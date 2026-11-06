//
// Created by luocf on 2019/6/13.
//
#include <cstring>
#include <algorithm>
#include <cctype>
#include <future>
#include <stack>
#include <iostream>
#include <ctime>

using namespace std;

#include "CarrierCompat.h"
#include "CarrierCompat.h"
#include <carrier_session.h>
#include <carrier_session_deprecated.h>
#include <thread>
#include <chrono>
#include <Log.hpp>
#include <json.hpp>
#include <CommonVar.h>
#include <ThirdParty/CompatibleFileSystem.hpp>
#include <Command/ChatRobotCmd.hpp>
#include "CarrierRobot.h"
#include "ErrCode.h"

namespace chatrobot {
    using json = nlohmann::json;
    std::string CarrierRobot::Factory::sLocalDataDir;

    namespace {
        constexpr const char *kAgentOutboundPrefix = "CGP1 ";
        constexpr const char *kAgentReplyPrefix = "CGR1 ";
        constexpr const char *kAgentStatusPrefix = "CGS1 ";

        // File message (docs/GROUP_FILE_MESSAGE.md): one friend message
        //   [u32 BE metaLen][meta JSON {"type":"file",...}][raw bytes]
        // the same envelope clients send 1:1 to an offline friend. Binary: it
        // must never pass through C-string handling or the "nick: text" wrap.
        constexpr size_t kFileMetaMaxLen = 4096;
        // Whole-message cap for what the group relays as a file: the core will
        // not send more than this to a legacy-protocol member (Android, JS).
        constexpr size_t kFileMessageMaxLen = CARRIER_LEGACY_MAX_APP_BULKMSG_LEN;

        enum class FileMessageKind { NotFile, File, Invalid };

        // Standard base64 (RFC 4648, with padding) for the file bytes an agent
        // gets inside its CGP1 envelope (message.file.data_base64).
        std::string base64Encode(const char *data, size_t len) {
            static const char kAlphabet[] =
                    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            const unsigned char *b = reinterpret_cast<const unsigned char *>(data);
            std::string out;
            out.reserve(((len + 2) / 3) * 4);
            size_t i = 0;
            for (; i + 2 < len; i += 3) {
                const unsigned int v = (b[i] << 16) | (b[i + 1] << 8) | b[i + 2];
                out.push_back(kAlphabet[(v >> 18) & 0x3F]);
                out.push_back(kAlphabet[(v >> 12) & 0x3F]);
                out.push_back(kAlphabet[(v >> 6) & 0x3F]);
                out.push_back(kAlphabet[v & 0x3F]);
            }
            if (i < len) {
                unsigned int v = b[i] << 16;
                if (i + 1 < len) {
                    v |= b[i + 1] << 8;
                }
                out.push_back(kAlphabet[(v >> 18) & 0x3F]);
                out.push_back(kAlphabet[(v >> 12) & 0x3F]);
                out.push_back(i + 1 < len ? kAlphabet[(v >> 6) & 0x3F] : '=');
                out.push_back('=');
            }
            return out;
        }

        // Reader rules of the client spec (iOS ChatOfflineFileWire.unpack):
        // metaLen 1..4096, meta is a JSON object with type "file", and a
        // declared size must fit. Invalid = it claims to be a file message
        // (two leading zero bytes, sane length) but is truncated/oversized.
        FileMessageKind parseFileMessage(const std::string &raw, json *meta_out,
                                         size_t *meta_len_out) {
            if (raw.size() < 5
                || raw[0] != '\0' || raw[1] != '\0') {
                return FileMessageKind::NotFile;
            }
            const unsigned char *b = reinterpret_cast<const unsigned char *>(raw.data());
            const size_t meta_len = (static_cast<size_t>(b[0]) << 24) | (static_cast<size_t>(b[1]) << 16)
                                    | (static_cast<size_t>(b[2]) << 8) | static_cast<size_t>(b[3]);
            if (meta_len < 1 || meta_len > kFileMetaMaxLen || 4 + meta_len > raw.size()) {
                return FileMessageKind::NotFile;
            }
            json meta;
            try {
                meta = json::parse(raw.begin() + 4, raw.begin() + 4 + meta_len);
            } catch (...) {
                return FileMessageKind::NotFile;
            }
            if (!meta.is_object()) {
                return FileMessageKind::NotFile;
            }
            auto type_it = meta.find("type");
            if (type_it == meta.end() || !type_it->is_string() || type_it->get<std::string>() != "file") {
                return FileMessageKind::NotFile;
            }
            auto size_it = meta.find("size");
            if (size_it != meta.end() && size_it->is_number_unsigned()
                && 4 + meta_len + size_it->get<uint64_t>() > raw.size()) {
                return FileMessageKind::Invalid;
            }
            if (raw.size() > kFileMessageMaxLen) {
                return FileMessageKind::Invalid;
            }
            if (meta_out != nullptr) {
                *meta_out = std::move(meta);
            }
            if (meta_len_out != nullptr) {
                *meta_len_out = meta_len;
            }
            return FileMessageKind::File;
        }

        // meta[key] if it is a string, else fallback. json::value() throws on a
        // non-string value, and the relay runs inside a Carrier callback.
        std::string metaString(const json &meta, const char *key, const std::string &fallback) {
            auto it = meta.find(key);
            return it != meta.end() && it->is_string() ? it->get<std::string>() : fallback;
        }

        // A DNFT1 streamed-transfer frame (0x1E "DNFT1"). It is a 1:1
        // handshake that needs the receiver to answer; relayed into a group it
        // only ever became junk text for every member (2026-10-09).
        bool isDnft1Frame(const std::string &raw) {
            return raw.size() >= 6 && raw[0] == '\x1e' && raw.compare(1, 5, "DNFT1") == 0;
        }

        // [u32 BE len(meta_str)][meta_str][the file bytes of raw, after its own meta]
        std::string packFileMessage(const std::string &meta_str, const std::string &raw,
                                    size_t old_meta_len) {
            const size_t body_off = 4 + old_meta_len;
            std::string out;
            out.reserve(4 + meta_str.size() + raw.size() - body_off);
            const uint32_t n = static_cast<uint32_t>(meta_str.size());
            out.push_back(static_cast<char>((n >> 24) & 0xff));
            out.push_back(static_cast<char>((n >> 16) & 0xff));
            out.push_back(static_cast<char>((n >> 8) & 0xff));
            out.push_back(static_cast<char>(n & 0xff));
            out.append(meta_str);
            out.append(raw, body_off, std::string::npos);
            return out;
        }

        // Carrier profile and message data is supplied by remote peers.  Do not
        // let a malformed byte sequence from one peer terminate the whole group
        // service while serializing a relay envelope.
        std::string dumpJsonForTransport(const json &value) {
            return value.dump(-1, ' ', false, json::error_handler_t::replace);
        }

        std::string trimDisplayName(const std::string &value) {
            size_t begin = 0;
            while (begin < value.size()
                   && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
                ++begin;
            }
            size_t end = value.size();
            while (end > begin
                   && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
                --end;
            }
            return value.substr(begin, end - begin);
        }

        std::string buildFallbackUserName(const char *friendid) {
            if (friendid == nullptr || friendid[0] == '\0') {
                return "CarrierUser";
            }

            std::string friend_id(friendid);
            return std::string("CarrierUser-")
                   + friend_id.substr(0, std::min<size_t>(8, friend_id.size()));
        }

        bool startsWith(const std::string &value, const char *prefix) {
            const size_t prefix_len = strlen(prefix);
            return value.size() >= prefix_len
                   && value.compare(0, prefix_len, prefix) == 0;
        }

        std::string normalizeProtocolToken(const std::string &value) {
            std::string normalized;
            normalized.reserve(value.size());
            for (char ch : value) {
                unsigned char uch = static_cast<unsigned char>(ch);
                if (std::isalnum(uch) == 0) {
                    continue;
                }
                normalized.push_back(static_cast<char>(std::tolower(uch)));
            }
            return normalized;
        }

        std::string sanitizeJsonPayload(const std::string &value) {
            std::string sanitized;
            sanitized.reserve(value.size());
            for (char ch : value) {
                unsigned char uch = static_cast<unsigned char>(ch);
                if (uch < 0x20 && ch != '\t' && ch != '\n' && ch != '\r') {
                    continue;
                }
                sanitized.push_back(ch);
            }
            return sanitized;
        }

        const char *connectionStatusToString(ElaConnectionStatus status) {
            switch (status) {
                case ElaConnectionStatus_Connected:
                    return "connected";
                case ElaConnectionStatus_Disconnected:
                    return "disconnected";
                default:
                    return "unknown";
            }
        }

        const char *presenceStatusToString(ElaPresenceStatus presence) {
            switch (presence) {
                case ElaPresenceStatus_None:
                    return "none";
                case ElaPresenceStatus_Away:
                    return "away";
                case ElaPresenceStatus_Busy:
                    return "busy";
                default:
                    return "unknown";
            }
        }

        json buildUserInfoJson(const ElaUserInfo &user_info) {
            json user;
            user["userid"] = user_info.userid;
            user["name"] = user_info.name;
            user["description"] = user_info.description;
            user["has_avatar"] = user_info.has_avatar;
            user["gender"] = user_info.gender;
            user["phone"] = user_info.phone;
            user["email"] = user_info.email;
            user["region"] = user_info.region;
            return user;
        }

        // AgentNet proto v2: what the sender's client advertised and the profile
        // it published. Members only see each other through this envelope, so
        // without it a group is a profile black hole. Empty for legacy peers.
        json buildAgentNetJson(ElaCarrier *ela, const char *userid) {
            Carrier *carrier = reinterpret_cast<Carrier *>(ela);
            json agentnet;
            CarrierClientInfo client;
            CarrierProfileExt ext;
            if (carrier && userid && carrier_get_friend_client_info(carrier, userid, &client) == 0) {
                agentnet["proto_version"] = client.proto_version;
                agentnet["platform"] = client.platform;
                agentnet["os_version"] = client.os_version;
                agentnet["app_version"] = client.app_version;
            }
            if (carrier && userid && carrier_get_friend_profile_ext(carrier, userid, &ext) == 0) {
                agentnet["avatar_url"] = ext.avatar_url;
                agentnet["url"] = ext.url;
                agentnet["ens"] = ext.ens;
                agentnet["extra"] = ext.extra;
            }
            return agentnet;
        }

        json buildFriendInfoJson(const ElaFriendInfo &friend_info) {
            json info;
            info["label"] = friend_info.label;
            info["status"] = static_cast<int>(friend_info.status);
            info["status_text"] = connectionStatusToString(friend_info.status);
            info["presence"] = static_cast<int>(friend_info.presence);
            info["presence_text"] = presenceStatusToString(friend_info.presence);
            return info;
        }
    }

    /***********************************************/
    /***** static function implement ***************/
    /***********************************************/
    int CarrierRobot::Factory::SetLocalDataDir(const std::string &dir) {
        if (dir.empty()) {
            return ErrCode::InvalidArgument;
        }

        std::error_code stdErrCode;
        bool ret;
        ret = std::filesystem::create_directories(dir, stdErrCode);
        if (ret == false
            || stdErrCode.value() != 0) {
            int errCode = ErrCode::StdSystemErrorIndex - stdErrCode.value();
            auto errMsg = ErrCode::ToString(errCode);
            Log::D(Log::TAG, "Failed to set local data dir, errcode: %s", errMsg.c_str());
            return errCode;
        }

        sLocalDataDir = dir;

        return 0;
    }

    std::shared_ptr<CarrierRobot> CarrierRobot::Factory::Create() {
        struct Impl : CarrierRobot {
        };

        return std::make_shared<Impl>();
    }

    CarrierRobot::~CarrierRobot() {
        mCarrierConfig.reset();
        this->stop();
    }

    CarrierRobot::CarrierRobot() {
        mCarrierConfig = std::make_shared<CarrierConfig>();
        mDatabaseProxy = std::make_shared<DatabaseProxy>();
        //anypeer Id format
        std::string reg_str("(\\w{8})-(\\w{4})-(\\w{4})-(\\w{4})-(\\w{13})");
        mMsgReg = std::make_shared<std::regex>(reg_str, std::regex::icase);
        mStatus = 0;
    }

    int CarrierRobot::GetCarrierUsrIdByAddress(const std::string &address, std::string &usrId) {
        char buf[ELA_MAX_ID_LEN + 1] = {0};
        auto ret = ela_get_id_by_address(address.c_str(), buf, sizeof(buf));
        if (ret == nullptr) {
            int err = ela_get_error();
            char strerr_buf[512] = {0};
            ela_get_strerror(err, strerr_buf, sizeof(strerr_buf));
            Log::E(Log::TAG, "Failed to add friend! ret=%s(0x%x)", strerr_buf, ret);
            return ErrCode::FailedCarrier;
        }

        usrId = ret;
        return 0;
    }

    void CarrierRobot::runCarrierInner() {
        int ret = ela_run(mCarrier.get(), 500);
        if (ret < 0) {
            ela_kill(mCarrier.get());
            Log::E(Log::TAG, "Failed to run carrier!");
        }

    }

    void CarrierRobot::runCarrier() {
        mCarrieryThread = std::thread(&CarrierRobot::runCarrierInner, this); //引用
    }

    std::shared_ptr<std::string> CarrierRobot::resolveGroupDisplayName(int service_id) {
        std::string display_name;
        auto group_nickname = mDatabaseProxy->getGroupNickName();
        if (group_nickname.get() != nullptr) {
            display_name = trimDisplayName(*group_nickname.get());
        }

        if (display_name.empty()) {
            std::string base_name = "CarrierGroup";
            if (mCarrierConfig.get() != nullptr
                && mCarrierConfig->NickName.get() != nullptr) {
                std::string configured_name = trimDisplayName(*mCarrierConfig->NickName.get());
                if (!configured_name.empty()) {
                    base_name = configured_name;
                }
            }
            display_name = base_name + "-" + std::to_string(service_id);
        }

        if (display_name.size() > ELA_MAX_USER_NAME_LEN) {
            display_name.resize(ELA_MAX_USER_NAME_LEN);
        }
        return std::make_shared<std::string>(display_name);
    }

    std::shared_ptr<std::string> CarrierRobot::resolveMemberDisplayName(const char *friendid,
                                                                         const char *display_name) {
        std::string normalized_name;
        if (display_name != nullptr) {
            normalized_name = trimDisplayName(std::string(display_name));
        }
        if (normalized_name.empty()) {
            normalized_name = buildFallbackUserName(friendid);
        }
        if (normalized_name.size() > ELA_MAX_USER_NAME_LEN) {
            normalized_name.resize(ELA_MAX_USER_NAME_LEN);
        }
        return std::make_shared<std::string>(normalized_name);
    }

    int CarrierRobot::applySelfDisplayName(int service_id) {
        std::shared_ptr<std::string> display_name = resolveGroupDisplayName(service_id);
        if (display_name.get() == nullptr || display_name->empty()) {
            return 0;
        }

        ElaUserInfo self_info;
        memset(&self_info, 0, sizeof(self_info));
        int ret = ela_get_self_info(mCarrier.get(), &self_info);
        if (ret != 0) {
            Log::I(TAG, "applySelfDisplayName failed to get self info, errno:(0x%x)", ela_get_error());
            return ret;
        }

        if (std::string(self_info.name) == *display_name.get()) {
            return 0;
        }

        strncpy(self_info.name, display_name->c_str(), ELA_MAX_USER_NAME_LEN);
        self_info.name[ELA_MAX_USER_NAME_LEN] = '\0';
        ret = ela_set_self_info(mCarrier.get(), &self_info);
        if (ret != 0) {
            Log::I(TAG, "applySelfDisplayName failed to set self info, errno:(0x%x)", ela_get_error());
            return ret;
        }

        mDatabaseProxy->updateGroupNickName(display_name);
        return 0;
    }

    void CarrierRobot::OnCarrierFriendInfoChanged(ElaCarrier *carrier, const char *friendid,
                                                  const ElaFriendInfo *info, void *context) {
        auto carrier_robot = reinterpret_cast<CarrierRobot *>(context);
        const char *raw_name = info != nullptr ? info->user_info.name : nullptr;
        std::shared_ptr<std::string> nickanme = carrier_robot->resolveMemberDisplayName(friendid,
                                                                                         raw_name);
        carrier_robot->updateMemberInfo(std::make_shared<std::string>(friendid), nickanme,
                                        ElaConnectionStatus_Connected);
    }

    void CarrierRobot::OnCarrierConnection(ElaCarrier *carrier,
                                           ElaConnectionStatus status, void *context) {
        auto channel = reinterpret_cast<CarrierRobot *>(context);
        Log::I(Log::TAG, "OnCarrierConnection status: %d", status);
        std::string carrierAddr, carrierUsrId;
        channel->getAddress(carrierAddr);
        GetCarrierUsrIdByAddress(carrierAddr, carrierUsrId);
    }

    void CarrierRobot::OnCarrierFriendRequest(ElaCarrier *carrier, const char *friendid,
                                              const ElaUserInfo *info,
                                              const char *hello, void *context) {
        Log::I(Log::TAG, "OnCarrierFriendRequest from: %s", friendid);
        auto carrierRobot = reinterpret_cast<CarrierRobot *>(context);
        carrierRobot->acceptFriend(std::make_shared<std::string>(friendid));
    }

    std::time_t CarrierRobot::getTimeStamp() {
        auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        return static_cast<std::time_t>(now_us);
    }

    void CarrierRobot::OnCarrierFriendConnection(ElaCarrier *carrier, const char *friendid,
                                                 ElaConnectionStatus status, void *context) {
        Log::I(Log::TAG, "OnCarrierFriendConnection from: %s %d", friendid, status);
        auto carrier_robot = reinterpret_cast<CarrierRobot *>(context);
        if (!carrier_robot->inRemovedList(std::make_shared<std::string>(friendid))) {
            ElaFriendInfo info;
            int ret = ela_get_friend_info(carrier, friendid, &info);
            const char *raw_name = ret == 0 ? info.user_info.name : nullptr;
            std::shared_ptr<std::string> nickanme = carrier_robot->resolveMemberDisplayName(friendid,
                                                                                             raw_name);
            carrier_robot->updateMemberInfo(std::make_shared<std::string>(friendid), nickanme, status);

        }
    }

    bool CarrierRobot::inRemovedList(std::shared_ptr<std::string> friendid) {
        return mDatabaseProxy->inRemovedList(*friendid.get());
    }

    void CarrierRobot::updateMemberInfo(std::shared_ptr<std::string> friendid,
                                        std::shared_ptr<std::string> nickname,
                                        ElaConnectionStatus status) {
        if (mCreaterFriendId.get() == nullptr) {
            mCreaterFriendId = friendid;
        }

        std::shared_ptr<MemberInfo> block_member_info = mDatabaseProxy->getBlockMemberInfo(friendid);
        if (block_member_info.get() == nullptr) {
            mDatabaseProxy->updateMemberInfo(friendid, nickname, status == ElaConnectionStatus_Connected ? 0 : 1, 0);
            //当前状态为上线时，获取该成员offline以后的所以消息，并发送给该人,
            if (status == ElaConnectionStatus_Connected) {
                relayMessages();
            }
        }
        json msg;
        msg["cmd"] = Command_UpdateMemberCount;
        msg["friendid"] = mAddress.c_str();
        msg["membercount"] = getFriendList()->size();
        sendMsgForManager(dumpJsonForTransport(msg));
    }

    bool CarrierRobot::relayMessages() {
        MUTEX_LOCKER locker_sync_data(_mReplyMessage);
        std::shared_ptr<std::vector<std::shared_ptr<MemberInfo>>> memberlist = mDatabaseProxy->getFriendList();
        for (int i = 0; i < memberlist->size(); i++) {
            std::shared_ptr<MemberInfo> memberInfo = memberlist->at(i);
            if (memberInfo.get() == nullptr) {
                continue;
            }
            bool info_changed = false;
            memberInfo->Lock();
            if (memberInfo->mStatus != 0) {
                memberInfo->UnLock();
                continue;
            }
            std::shared_ptr<std::vector<std::shared_ptr<MessageInfo>>> message_list = mDatabaseProxy->getMessages(
                    memberInfo->mFriendid, memberInfo->mMsgTimeStamp, 100);
            if (message_list.get() != nullptr) {
                for (int i = 0; i < message_list->size(); i++) {
                    std::shared_ptr<MessageInfo> message = message_list->at(i);
                    if (message.get() != nullptr) {
                        if (memberInfo->mFriendid.get() != nullptr
                            && message->mFriendid.get() != nullptr
                            && mDatabaseProxy->hasAgent(*memberInfo->mFriendid.get())
                            && memberInfo->mFriendid->compare(*message->mFriendid.get()) == 0) {
                            // Skip echoing an agent's own message back to itself.
                            info_changed = true;
                            memberInfo->mMsgTimeStamp = message->mSendTimeStamp;
                            continue;
                        }
                        std::string msg = buildOutboundMessageForRecipient(
                                *memberInfo->mFriendid.get(), message);
                        if (msg.empty()) {
                            info_changed = true;
                            memberInfo->mMsgTimeStamp = message->mSendTimeStamp;
                            continue;
                        }
                        int msg_ret = ela_send_friend_message_noreceipt(mCarrier.get(),
                                                              memberInfo->mFriendid.get()->c_str(),
                                                              msg.data(), msg.size());
                        if (msg_ret != 0) {
                            const int err = ela_get_error();
                            Log::I(TAG,
                                   "relayMessages send failed to=%s from=%s ts=%lld len=%zu err=0x%x",
                                   memberInfo->mFriendid.get() != nullptr
                                   ? memberInfo->mFriendid->c_str() : "",
                                   message->mFriendid.get() != nullptr
                                   ? message->mFriendid->c_str() : "",
                                   static_cast<long long>(message->mSendTimeStamp),
                                   msg.size(), err);
                            if (err == CARRIER_GENERAL_ERROR(ERROR_INVALID_ARGS)) {
                                // Permanent for this member (e.g. over a legacy
                                // peer's 5 MB cap): skip it, or the member is
                                // stuck behind this message forever.
                                info_changed = true;
                                memberInfo->mMsgTimeStamp = message->mSendTimeStamp;
                                continue;
                            }
                            break;
                        }
                        info_changed = true;
                        memberInfo->mMsgTimeStamp = message->mSendTimeStamp;
                    }
                }
            }
            memberInfo->UnLock();
            if (info_changed) {
                mDatabaseProxy->updateMemberInfo(memberInfo->mFriendid, memberInfo->mNickName, memberInfo->mStatus, memberInfo->mMsgTimeStamp);
            }

        }
        return false;
    }

    namespace {
        // The core's timestamp for an Express message, as microseconds. The
        // node stamps it in us; accept s/ms too rather than trust one unit.
        int64_t normalizeToMicros(int64_t ts) {
            if (ts <= 0) return 0;
            if (ts < 100000000000LL) return ts * 1000000LL;      // seconds
            if (ts < 100000000000000LL) return ts * 1000LL;      // milliseconds
            return ts;                                           // microseconds
        }
    }

    bool CarrierRobot::acceptOfflineCopy(const std::string &friend_id, const std::string &stored_form,
                                         std::time_t recv_time, int64_t post_time) {
        constexpr int64_t kMinute = 60LL * 1000000LL;
        int64_t posted = normalizeToMicros(post_time);
        if (posted > static_cast<int64_t>(recv_time) + 60 * kMinute) {
            posted = 0;  // nonsense from the node: judge by arrival only
        }
        // Parked before this group pulled Express at all (an hour of slack
        // for node clocks): delivered weeks late into a busy group it would
        // only confuse. Kept in express_backlog_table, not relayed.
        if (posted > 0 && mExpressSince > 0 && posted < static_cast<int64_t>(mExpressSince) - 60 * kMinute) {
            // The nodes hand the same message out again on every pull (11-03:
            // the core's watermark never passes them), so keep it once.
            if (mDatabaseProxy->addExpressBacklog(friend_id, stored_form, static_cast<std::time_t>(posted), recv_time)) {
                Log::I(TAG, "addMessgae express backlog kept, not relayed: from=%s len=%zu posted_us=%lld",
                       friend_id.c_str(), stored_form.size(), static_cast<long long>(posted));
            }
            return false;
        }
        // The core re-sends a live message through Express when its receipt
        // does not come back in 30 s, so a message can arrive twice, and the
        // nodes re-deliver the same parked message on every pull. Same
        // sender, same bytes, stored since 10 min before it was posted = the
        // copy we already have. The post time is fixed per message, so a
        // re-delivery weeks later still matches. Without a post time: a day.
        int64_t since = posted > 0
                        ? std::min<int64_t>(posted, recv_time) - 10 * kMinute
                        : static_cast<int64_t>(recv_time) - 24 * 60 * kMinute;
        if (mDatabaseProxy->hasMessageSince(friend_id, stored_form, static_cast<std::time_t>(since))) {
            Log::I(TAG, "addMessgae express duplicate dropped: from=%s len=%zu", friend_id.c_str(),
                   stored_form.size());
            return false;
        }
        return true;
    }

    void CarrierRobot::addMessgae(std::shared_ptr<std::string> friend_id,
                                  std::shared_ptr<std::string> message, std::time_t send_time,
                                  bool offline, int64_t post_time) {
        std::string errMsg;
        if (isDnft1Frame(*message.get())) {
            Log::I(TAG, "addMessgae drop DNFT1 frame from=%s len=%zu", friend_id->c_str(),
                   message->size());
            return;
        }
        switch (parseFileMessage(*message.get(), nullptr, nullptr)) {
            case FileMessageKind::File:
                // Stored and relayed byte for byte; never a command or agent reply.
                Log::I(TAG, "addMessgae file message from=%s len=%zu offline=%d", friend_id->c_str(),
                       message->size(), offline ? 1 : 0);
                if (offline && !acceptOfflineCopy(*friend_id, *message.get(), send_time, post_time)) {
                    return;
                }
                mDatabaseProxy->addMessgae(friend_id, *message.get(), send_time);
                relayMessages();
                return;
            case FileMessageKind::Invalid:
                Log::I(TAG, "addMessgae drop truncated/oversized file message from=%s len=%zu",
                       friend_id->c_str(), message->size());
                return;
            case FileMessageKind::NotFile:
                break;
        }
        std::string msg = normalizeIncomingMessageForStorage(*friend_id.get(), *message.get());
        std::string status_message = normalizeAgentStatusEnvelope(*friend_id.get(), *message.get());
        if (!status_message.empty()) {
            relayAgentStatusToMembers(*friend_id.get(), status_message);
            return;
        }
        if (msg.size() > 38 &&
            std::regex_match(msg.substr(0, 37).c_str(), *mMsgReg.get()) == true) {
            msg = msg.substr(38);//包含空格
        }
        // Checked before commands too: a /command parked weeks ago must not run now.
        if (offline && !acceptOfflineCopy(*friend_id, msg, send_time, post_time)) {
            return;
        }
        std::string pre_cmd = msg + " " + *friend_id.get();//Pretreatment cmd
        int ret = ChatRobotCmd::Do(this, pre_cmd, errMsg);
        if (ret < 0) {
            //save message
            mDatabaseProxy->addMessgae(friend_id, msg, send_time);
            //将该消息转发给其他人
            relayMessages();
        }
    }

    std::shared_ptr<std::vector<std::shared_ptr<MemberInfo>>> CarrierRobot::getFriendList() {
        return mDatabaseProxy->getFriendList();
    }

    void CarrierRobot::OnCarrierFriendMessage(ElaCarrier *carrier, const char *from,
                                              const void *msg, size_t len,
                                              int64_t timestamp, bool offline,
                                              void *context) {
        Log::I(Log::TAG, "OnCarrierFriendMessage from: %s len=%zu offline=%d ts=%lld", from, len,
               offline ? 1 : 0, static_cast<long long>(timestamp));
        auto carrier_robot = reinterpret_cast<CarrierRobot *>(context);
        // Carrier payloads are not NUL-terminated; take exactly len bytes and drop trailing NULs
        // (fix carried on the live box since 2026-09-14, folded in here). A file message is
        // binary and may legitimately end in 0x00, so it is kept as is.
        std::string text((const char *) msg, len);
        if (parseFileMessage(text, nullptr, nullptr) == FileMessageKind::NotFile) {
            while (!text.empty() && text.back() == '\0') text.pop_back();
        }
        std::time_t send_time = carrier_robot->getTimeStamp();
        carrier_robot->addMessgae(std::make_shared<std::string>(from),
                                  std::make_shared<std::string>(text),
                                  send_time, offline, timestamp);
    }

    void CarrierRobot::stop() {
        mDatabaseProxy->closeDb();
    }

    int CarrierRobot::start(const char *data_dir, int service_id, int my_socket_fd) {
        mMySocketFd = my_socket_fd;
        ElaOptions carrierOpts;
        ElaCallbacks carrierCallbacks;
        mLocalPath = std::make_shared<std::string>(data_dir);
        mDatabaseProxy->startDb(data_dir);
        memset(&carrierOpts, 0, sizeof(carrierOpts));
        memset(&carrierCallbacks, 0, sizeof(carrierCallbacks));
        carrierCallbacks.connection_status = OnCarrierConnection;
        carrierCallbacks.friend_request = OnCarrierFriendRequest;
        carrierCallbacks.friend_connection = OnCarrierFriendConnection;
        carrierCallbacks.friend_message = OnCarrierFriendMessage;
        carrierCallbacks.friend_info = OnCarrierFriendInfoChanged;
        carrierOpts.udp_enabled = mCarrierConfig->mEnableUdp;
        carrierOpts.persistent_location = data_dir;

        // set BootstrapNode
        size_t carrierNodeSize = mCarrierConfig->mBootstrapNodes.size();
        BootstrapNode carrierNodeArray[carrierNodeSize];

        memset(carrierNodeArray, 0, sizeof(carrierNodeArray));
        for (int idx = 0; idx < carrierNodeSize; idx++) {
            const auto &node = mCarrierConfig->mBootstrapNodes[idx];
            carrierNodeArray[idx].ipv4 = strdup(node->mIpv4->c_str());
            carrierNodeArray[idx].ipv6 = NULL;
            carrierNodeArray[idx].port = strdup(node->mPort->c_str());
            carrierNodeArray[idx].public_key = strdup(node->mPublicKey->c_str());
        }
        carrierOpts.bootstraps_size = carrierNodeSize;
        carrierOpts.bootstraps = carrierNodeArray;

        size_t expressNodeSize = mCarrierConfig->mExpressNodes.size();
        std::vector<ExpressNode> expressNodeArray(expressNodeSize);
        for (size_t idx = 0; idx < expressNodeSize; idx++) {
            const auto &node = mCarrierConfig->mExpressNodes[idx];
            expressNodeArray[idx].ipv4 = strdup(node->mIpv4->c_str());
            expressNodeArray[idx].ipv6 = NULL;
            expressNodeArray[idx].port = strdup(node->mPort->c_str());
            expressNodeArray[idx].public_key = strdup(node->mPublicKey->c_str());
        }
        carrierOpts.express_nodes_size = expressNodeSize;
        carrierOpts.express_nodes = expressNodeSize > 0 ? expressNodeArray.data() : nullptr;
        mExpressSince = mDatabaseProxy->getOrInitMeta("express_since_us", getTimeStamp());
        Log::I(TAG, "CarrierRobot::start express nodes=%zu express_since_us=%lld", expressNodeSize,
               static_cast<long long>(mExpressSince));
        ela_log_init(static_cast<ElaLogLevel>(mCarrierConfig->mLogLevel), nullptr, nullptr);

        auto creater = [&]() -> ElaCarrier * {
            ela_log_init(ElaLogLevel::ElaLogLevel_Warning, data_dir, nullptr);
            auto ptr = ela_new(&carrierOpts, &carrierCallbacks, this);
            return ptr;
        };

        auto deleter = [=](ElaCarrier *ptr) -> void {
            if (ptr != nullptr) {
                ela_session_cleanup(ptr);
                ela_kill(ptr);
            }
        };

        mCarrier = std::unique_ptr<ElaCarrier, std::function<void(ElaCarrier *)>>(creater(),
                deleter);
        if (mCarrier == nullptr) {
            Log::E(TAG, "Failed to new carrier!");
            int err = ela_get_error();
            char strerr_buf[512] = {0};
            ela_get_strerror(err, strerr_buf, sizeof(strerr_buf));
            Log::E(Log::TAG, "CarrierRobot::start failed! ret=%s(0x%x)", strerr_buf, err);
            return ErrCode::FailedCarrier;
        }

        int name_ret = applySelfDisplayName(service_id);
        if (name_ret != 0) {
            Log::I(TAG, "CarrierRobot::start continue with default self info, err=%d", name_ret);
        }
        std::shared_ptr<MemberInfo> member_info = mDatabaseProxy->getMemberInfo(1);
        if (member_info.get() != nullptr) {
            mCreaterFriendId = member_info->mFriendid;
        }
        getAddress(mAddress);
        json msg;
        msg["cmd"] = Command_UpdateAddress;
        msg["serviceid"] = service_id;
        msg["friendid"] = mAddress.c_str();
        sendMsgForManager(dumpJsonForTransport(msg));
        return 0;
    }

    int CarrierRobot::getUserId(std::string &userid) {
        char addr[ELA_MAX_ID_LEN + 1] = {0};
        auto ret = ela_get_userid(mCarrier.get(), addr, sizeof(addr));
        if (ret == nullptr) {
            int err = ela_get_error();
            char strerr_buf[512] = {0};
            ela_get_strerror(err, strerr_buf, sizeof(strerr_buf));
            return ErrCode::FailedCarrier;
        }
        userid = addr;
        return 0;
    }

    int CarrierRobot::acceptFriend(std::shared_ptr<std::string>friendid) {
        std::shared_ptr<MemberInfo> member_info = mDatabaseProxy->getBlockMemberInfo(friendid);
        if (member_info.get() == nullptr) {
            mDatabaseProxy->deleteRemovedListMember(*friendid.get());
            ela_accept_friend(mCarrier.get(), friendid->c_str());
        } else {
            Log::D(TAG, "acceptFriend can't accept friend, block member:%s", friendid->c_str());
        }

        return 0;
    }

    bool CarrierRobot::requestAgentFriend(const std::string &address, std::string &error_message) {
        std::string agent_user_id;
        int parse_ret = GetCarrierUsrIdByAddress(address, agent_user_id);
        if (parse_ret != 0 || agent_user_id.empty()) {
            error_message = "invalid carrier address";
            return false;
        }

        if (!mAddress.empty()
            && mAddress == address) {
            error_message = "cannot use the group address as agent";
            return false;
        }

        if (mDatabaseProxy->hasAgent(agent_user_id)) {
            mDatabaseProxy->addAgent(agent_user_id, address);
            return true;
        }

        std::shared_ptr<MemberInfo> agent_member = mDatabaseProxy->getMemberInfo(
                std::make_shared<std::string>(agent_user_id));
        if (agent_member.get() != nullptr) {
            // The target is already a group member: no friend request needed,
            // but the agent registration must still be recorded — returning
            // without addAgent() reported "Agent registered" while relay kept
            // sending plain text (agent_table never gained the userid).
            if (!mDatabaseProxy->addAgent(agent_user_id, address)) {
                error_message = "failed to persist agent registration";
                return false;
            }
            return true;
        }

        ElaFriendInfo carrier_friend_info;
        memset(&carrier_friend_info, 0, sizeof(carrier_friend_info));
        if (ela_get_friend_info(mCarrier.get(), agent_user_id.c_str(), &carrier_friend_info) == 0) {
            mDatabaseProxy->addAgent(agent_user_id, address);
            return true;
        }

        std::shared_ptr<MemberInfo> agent_block_member = mDatabaseProxy->getBlockMemberInfo(
                std::make_shared<std::string>(agent_user_id));
        if (agent_block_member.get() != nullptr) {
            error_message = "target agent user is blocked in this group";
            return false;
        }

        int add_ret = ela_add_friend(mCarrier.get(), address.c_str(), "openclaw agent invite");
        if (add_ret != 0) {
            int err = ela_get_error();
            char strerr_buf[512] = {0};
            ela_get_strerror(err, strerr_buf, sizeof(strerr_buf));
            error_message = std::string("failed to add agent friend: ") + strerr_buf;
            return false;
        }

        if (!mDatabaseProxy->addAgent(agent_user_id, address)) {
            error_message = "friend request sent, but failed to save agent metadata";
            return false;
        }
        return true;
    }

    int CarrierRobot::getAddress(std::string &address) {
        char addr[ELA_MAX_ADDRESS_LEN + 1] = {0};
        auto ret = ela_get_address(mCarrier.get(), addr, sizeof(addr));
        if (ret == nullptr) {
            int err = ela_get_error();
            char strerr_buf[512] = {0};
            ela_get_strerror(err, strerr_buf, sizeof(strerr_buf));

            return ErrCode::FailedCarrier;
        }

        address = addr;
        return 0;
    }

    std::string CarrierRobot::convertDatetimeToString(std::time_t time) {
        std::time_t normalized_time = time;
        if (normalized_time > 100000000000000LL) {
            normalized_time /= 1000000LL;
        } else if (normalized_time > 100000000000LL) {
            normalized_time /= 1000LL;
        }
        tm *tm_ = localtime(&normalized_time);                // 将time_t格式转换为tm结构体
        int year, month, day, hour, minute, second;// 定义时间的各个int临时变量。
        year = tm_->tm_year +
               1900;                // 临时变量，年，由于tm结构体存储的是从1900年开始的时间，所以临时变量int为tm_year加上1900。
        month = tm_->tm_mon +
                1;                   // 临时变量，月，由于tm结构体的月份存储范围为0-11，所以临时变量int为tm_mon加上1。
        day = tm_->tm_mday;                        // 临时变量，日。
        hour = tm_->tm_hour;                       // 临时变量，时。
        minute = tm_->tm_min;                      // 临时变量，分。
        second = tm_->tm_sec;                      // 临时变量，秒。
        char yearStr[5], monthStr[3], dayStr[3], hourStr[3], minuteStr[3], secondStr[3];// 定义时间的各个char*变量。
        sprintf(yearStr, "%d", year);              // 年。
        sprintf(monthStr, "%d", month);            // 月。
        sprintf(dayStr, "%d", day);                // 日。
        sprintf(hourStr, "%d", hour);              // 时。
        sprintf(minuteStr, "%d", minute);          // 分。
        if (minuteStr[1] == '\0')                  // 如果分为一位，如5，则需要转换字符串为两位，如05。
        {
            minuteStr[2] = '\0';
            minuteStr[1] = minuteStr[0];
            minuteStr[0] = '0';
        }
        sprintf(secondStr, "%d", second);          // 秒。
        if (secondStr[1] == '\0')                  // 如果秒为一位，如5，则需要转换字符串为两位，如05。
        {
            secondStr[2] = '\0';
            secondStr[1] = secondStr[0];
            secondStr[0] = '0';
        }
        char s[20];                                // 定义总日期时间char*变量。
        sprintf(s, "%s-%s-%s %s:%s:%s", yearStr, monthStr, dayStr, hourStr, minuteStr,
                secondStr);// 将年月日时分秒合并。
        std::string str(s);                             // 定义string变量，并将总日期时间char*变量作为构造函数的参数传入。
        return str;                                // 返回转换日期时间后的string变量。
    }

    void CarrierRobot::sendCommandResponse(const std::string &friend_id, const std::string &message) {
        int ela_ret = ela_send_friend_message_noreceipt(mCarrier.get(), friend_id.c_str(),
                                              message.data(), message.size());
        if (ela_ret != 0) {
            Log::I(Log::TAG,
                   "sendCommandResponse .c_str(): %s errno:(0x%x)",
                   message.c_str(), ela_get_error());
        }
    }

    bool CarrierRobot::addAgentByAddress(const std::string &address, std::string &error_message) {
        return requestAgentFriend(address, error_message);
    }

    bool CarrierRobot::removeAgentByUserId(const std::string &user_id, std::string &error_message) {
        if (!mDatabaseProxy->hasAgent(user_id)) {
            error_message = "agent userid not found in this group";
            return false;
        }

        int remove_ret = ela_remove_friend(mCarrier.get(), user_id.c_str());
        bool db_removed = mDatabaseProxy->removeAgent(user_id);
        if (!db_removed) {
            error_message = "failed to remove agent metadata";
            return false;
        }

        if (remove_ret != 0) {
            error_message = std::string("carrier remove returned error: ")
                            + std::to_string(ela_get_error());
            return false;
        }
        return true;
    }

    std::shared_ptr<std::vector<std::shared_ptr<std::string>>> CarrierRobot::getAgentUserIdList() {
        return mDatabaseProxy->getAgentUserIdList();
    }

    std::shared_ptr<std::string> CarrierRobot::getAgentAddressByUserId(const std::string &user_id) {
        return mDatabaseProxy->getAgentAddress(user_id);
    }

    std::string CarrierRobot::normalizeIncomingMessageForStorage(const std::string &friend_id,
                                                                 const std::string &message) {
        if (!mDatabaseProxy->hasAgent(friend_id)) {
            return message;
        }
        if (!startsWith(message, kAgentReplyPrefix)) {
            return message;
        }

        const std::string raw_payload = message.substr(strlen(kAgentReplyPrefix));
        try {
            json payload;
            try {
                payload = json::parse(raw_payload);
            } catch (...) {
                payload = json::parse(sanitizeJsonPayload(raw_payload));
            }

            auto type_it = payload.find("type");
            if (type_it != payload.end()
                && type_it->is_string()) {
                const std::string type_value = normalizeProtocolToken(type_it->get<std::string>());
                if (type_value != "carriergroupreply") {
                    return message;
                }
            }

            std::string chat_type_value;
            auto chat_type_it = payload.find("chat_type");
            if (chat_type_it != payload.end()
                && chat_type_it->is_string()) {
                chat_type_value = chat_type_it->get<std::string>();
            } else {
                auto compat_chat_type_it = payload.find("chattype");
                if (compat_chat_type_it != payload.end()
                    && compat_chat_type_it->is_string()) {
                    chat_type_value = compat_chat_type_it->get<std::string>();
                }
            }
            if (!chat_type_value.empty()
                && normalizeProtocolToken(chat_type_value) != "group") {
                return message;
            }

            auto group_it = payload.find("group");
            if (group_it != payload.end() && group_it->is_object()) {
                auto group_address_it = group_it->find("address");
                if (group_address_it != group_it->end()
                    && group_address_it->is_string()
                    && !mAddress.empty()
                    && group_address_it->get<std::string>() != mAddress) {
                    return message;
                }
            }

            std::string reply_text;
            auto message_it = payload.find("message");
            if (message_it != payload.end() && message_it->is_object()) {
                auto text_it = message_it->find("text");
                if (text_it != message_it->end() && text_it->is_string()) {
                    reply_text = text_it->get<std::string>();
                }
            } else if (message_it != payload.end() && message_it->is_string()) {
                reply_text = message_it->get<std::string>();
            }
            if (reply_text.empty()) {
                auto text_it = payload.find("text");
                if (text_it != payload.end() && text_it->is_string()) {
                    reply_text = text_it->get<std::string>();
                }
            }
            if (reply_text.empty()) {
                return message;
            }

            auto reply_to_it = payload.find("reply_to");
            if (reply_to_it != payload.end() && reply_to_it->is_object()) {
                auto user_id_it = reply_to_it->find("userid");
                if ((user_id_it == reply_to_it->end() || !user_id_it->is_string())) {
                    user_id_it = reply_to_it->find("userId");
                }
                if ((user_id_it == reply_to_it->end() || !user_id_it->is_string())) {
                    user_id_it = reply_to_it->find("friendid");
                }
                if (user_id_it != reply_to_it->end() && user_id_it->is_string()) {
                    std::shared_ptr<std::string> reply_user_id = std::make_shared<std::string>(user_id_it->get<std::string>());
                    std::shared_ptr<MemberInfo> target_member = mDatabaseProxy->getMemberInfo(reply_user_id);
                    if (target_member.get() != nullptr && target_member->mNickName.get() != nullptr) {
                        target_member->Lock();
                        std::string target_name = *target_member->mNickName.get();
                        target_member->UnLock();
                        if (!target_name.empty()) {
                            reply_text = std::string("@") + target_name + " " + reply_text;
                        }
                    }
                }
            }
            return reply_text;
        } catch (...) {
            return message;
        }
    }

    std::string CarrierRobot::normalizeAgentStatusEnvelope(const std::string &friend_id,
                                                           const std::string &message) {
        if (!mDatabaseProxy->hasAgent(friend_id)) {
            return "";
        }
        if (!startsWith(message, kAgentStatusPrefix)) {
            return "";
        }

        const std::string raw_payload = message.substr(strlen(kAgentStatusPrefix));
        if (raw_payload.empty()) {
            return "";
        }

        try {
            json payload;
            try {
                payload = json::parse(raw_payload);
            } catch (...) {
                payload = json::parse(sanitizeJsonPayload(raw_payload));
            }

            auto type_it = payload.find("type");
            if (type_it != payload.end() && type_it->is_string()) {
                const std::string type_value = normalizeProtocolToken(type_it->get<std::string>());
                if (type_value != "agentstatus"
                    && type_value != "carriergroupstatus") {
                    return "";
                }
            }

            std::string chat_type_value;
            auto chat_type_it = payload.find("chat_type");
            if (chat_type_it != payload.end() && chat_type_it->is_string()) {
                chat_type_value = chat_type_it->get<std::string>();
            } else {
                auto compat_chat_type_it = payload.find("chattype");
                if (compat_chat_type_it != payload.end() && compat_chat_type_it->is_string()) {
                    chat_type_value = compat_chat_type_it->get<std::string>();
                }
            }
            if (!chat_type_value.empty()
                && normalizeProtocolToken(chat_type_value) != "group") {
                return "";
            }

            auto group_it = payload.find("group");
            if (group_it != payload.end() && group_it->is_object()) {
                auto group_address_it = group_it->find("address");
                if (group_address_it != group_it->end()
                    && group_address_it->is_string()
                    && !mAddress.empty()
                    && group_address_it->get<std::string>() != mAddress) {
                    return "";
                }
            }

            auto status_it = payload.find("status");
            if (status_it == payload.end() || !status_it->is_object()) {
                return "";
            }
            auto state_it = status_it->find("state");
            if (state_it == status_it->end() || !state_it->is_string()) {
                return "";
            }
            std::string state = normalizeProtocolToken(state_it->get<std::string>());
            if (state.empty()) {
                return "";
            }

            json normalized;
            normalized["type"] = "agent_status";
            normalized["version"] = 1;
            normalized["chat_type"] = "group";
            normalized["source"] = "carrier_group_service";
            normalized["origin"]["userid"] = friend_id;
            normalized["origin"]["friendid"] = friend_id;
            normalized["group"]["address"] = mAddress;

            std::string group_user_id;
            if (getUserId(group_user_id) == 0) {
                normalized["group"]["userid"] = group_user_id;
            }

            std::shared_ptr<std::string> group_nickname = mDatabaseProxy->getGroupNickName();
            normalized["group"]["nickname"] = group_nickname.get() != nullptr ? *group_nickname.get() : "";

            if (group_it != payload.end() && group_it->is_object()) {
                auto group_user_id_it = group_it->find("userid");
                if (group_user_id_it != group_it->end() && group_user_id_it->is_string()) {
                    normalized["group"]["userid"] = group_user_id_it->get<std::string>();
                }
                auto group_name_it = group_it->find("name");
                if (group_name_it != group_it->end() && group_name_it->is_string()) {
                    normalized["group"]["nickname"] = group_name_it->get<std::string>();
                }
            }

            normalized["status"]["state"] = state;
            auto phase_it = status_it->find("phase");
            if (phase_it != status_it->end() && phase_it->is_string()) {
                normalized["status"]["phase"] = phase_it->get<std::string>();
            }
            auto ttl_it = status_it->find("ttl_ms");
            if (ttl_it != status_it->end() && ttl_it->is_number_integer()) {
                normalized["status"]["ttl_ms"] = ttl_it->get<int>();
            } else if (ttl_it != status_it->end() && ttl_it->is_string()) {
                try {
                    normalized["status"]["ttl_ms"] = std::stoi(ttl_it->get<std::string>());
                } catch (...) {
                    normalized["status"]["ttl_ms"] = 12000;
                }
            } else {
                normalized["status"]["ttl_ms"] = 12000;
            }
            auto seq_it = status_it->find("seq");
            if (seq_it != status_it->end() && seq_it->is_string()) {
                normalized["status"]["seq"] = seq_it->get<std::string>();
            }
            auto ts_it = status_it->find("ts");
            if (ts_it != status_it->end() && ts_it->is_number_integer()) {
                normalized["status"]["ts"] = ts_it->get<long long>();
            } else if (ts_it != status_it->end() && ts_it->is_string()) {
                try {
                    normalized["status"]["ts"] = std::stoll(ts_it->get<std::string>());
                } catch (...) {
                    normalized["status"]["ts"] = static_cast<long long>(std::time(nullptr));
                }
            } else {
                normalized["status"]["ts"] = static_cast<long long>(std::time(nullptr));
            }
            return std::string(kAgentStatusPrefix) + dumpJsonForTransport(normalized);
        } catch (...) {
            return "";
        }
    }

    bool CarrierRobot::relayAgentStatusToMembers(const std::string &agent_user_id,
                                                 const std::string &status_message) {
        std::shared_ptr<std::vector<std::shared_ptr<MemberInfo>>> member_list = mDatabaseProxy->getFriendList();
        if (member_list.get() == nullptr) {
            return false;
        }

        bool relayed = false;
        for (int i = 0; i < member_list->size(); i++) {
            std::shared_ptr<MemberInfo> member_info = member_list->at(i);
            if (member_info.get() == nullptr || member_info->mFriendid.get() == nullptr) {
                continue;
            }
            member_info->Lock();
            const std::string member_friend_id = *member_info->mFriendid.get();
            member_info->UnLock();
            if (member_friend_id.empty()
                || member_friend_id == agent_user_id
                || mDatabaseProxy->hasAgent(member_friend_id)) {
                continue;
            }

            int ela_ret = ela_send_friend_message_noreceipt(mCarrier.get(),
                                                  member_friend_id.c_str(),
                                                  status_message.data(),
                                                  status_message.size());
            if (ela_ret != 0) {
                Log::I(Log::TAG,
                       "relayAgentStatusToMembers failed: to=%s errno=(0x%x)",
                       member_friend_id.c_str(), ela_get_error());
                continue;
            }
            relayed = true;
        }
        return relayed;
    }

    std::string CarrierRobot::buildOutboundMessageForRecipient(const std::string &recipient_user_id,
                                                               std::shared_ptr<MessageInfo> message) {
        if (message.get() == nullptr || message->mMsg.get() == nullptr || message->mFriendid.get() == nullptr) {
            return "";
        }

        std::string sender_user_id = *message->mFriendid.get();
        std::string sender_nickname = buildFallbackUserName(sender_user_id.c_str());
        ElaFriendInfo sender_friend_info;
        bool has_sender_friend_info = false;
        memset(&sender_friend_info, 0, sizeof(sender_friend_info));
        if (ela_get_friend_info(mCarrier.get(), sender_user_id.c_str(), &sender_friend_info) == 0) {
            has_sender_friend_info = true;
            if (sender_friend_info.user_info.name[0] != '\0') {
                sender_nickname = sender_friend_info.user_info.name;
            }
        }

        std::shared_ptr<MemberInfo> sender_member_info = mDatabaseProxy->getMemberInfo(message->mFriendid);
        if (sender_member_info.get() != nullptr) {
            sender_member_info->Lock();
            if (sender_member_info->mNickName.get() != nullptr
                && !sender_member_info->mNickName->empty()) {
                sender_nickname = *sender_member_info->mNickName.get();
            }
            sender_member_info->UnLock();
        }

        json file_meta;
        size_t file_meta_len = 0;
        const bool is_file = parseFileMessage(*message->mMsg.get(), &file_meta, &file_meta_len)
                             == FileMessageKind::File;
        const bool to_agent = mDatabaseProxy->hasAgent(recipient_user_id);

        std::string group_user_id;
        getUserId(group_user_id);
        std::shared_ptr<std::string> group_nickname_ptr = mDatabaseProxy->getGroupNickName();
        const std::string group_nickname = group_nickname_ptr.get() != nullptr ? *group_nickname_ptr.get() : "";

        if (is_file && !to_agent) {
            // Same envelope the member sent, with the real sender added to the
            // meta (readers ignore keys they do not know). Whatever the sender
            // put under these keys is replaced, so it cannot spoof another member.
            static const char *kRelayKeys[] = {"chat_type", "group", "origin", "sent_at_ms"};
            for (const char *key : kRelayKeys) {
                file_meta.erase(key);
            }
            json relay_meta = file_meta;
            relay_meta["chat_type"] = "group";
            relay_meta["group"]["userid"] = group_user_id;
            relay_meta["group"]["address"] = mAddress;
            relay_meta["group"]["nickname"] = group_nickname;
            relay_meta["origin"]["userid"] = sender_user_id;
            relay_meta["origin"]["nickname"] = sender_nickname;
            relay_meta["sent_at_ms"] = static_cast<long long>(message->mSendTimeStamp / 1000);
            const size_t body_len = message->mMsg->size() - 4 - file_meta_len;
            std::string meta_str = dumpJsonForTransport(relay_meta);
            if (meta_str.size() > kFileMetaMaxLen
                || 4 + meta_str.size() + body_len > kFileMessageMaxLen) {
                // No room for the sender fields: relay the file without them.
                meta_str = dumpJsonForTransport(file_meta);
            }
            return packFileMessage(meta_str, *message->mMsg.get(), file_meta_len);
        }

        std::string body_text = *message->mMsg.get();
        if (is_file) {
            const std::string filename = metaString(file_meta, "filename", "file");
            body_text = std::string("[file] ") + filename + " ("
                        + std::to_string(message->mMsg->size() - 4 - file_meta_len) + " bytes)";
        }
        std::string plain_text = sender_nickname + ": " + body_text
                                 + " \n[" + this->convertDatetimeToString(message->mSendTimeStamp) + "]";
        if (!to_agent) {
            return plain_text;
        }

        json envelope;
        envelope["type"] = "carrier_group_message";
        envelope["version"] = 1;
        envelope["chat_type"] = "group";
        envelope["source"] = "carrier_group_service";
        envelope["group"]["address"] = mAddress;
        if (!group_user_id.empty()) {
            envelope["group"]["userid"] = group_user_id;
        }
        envelope["group"]["nickname"] = group_nickname;
        envelope["origin"]["userid"] = sender_user_id;
        envelope["origin"]["friendid"] = sender_user_id;
        envelope["origin"]["nickname"] = sender_nickname;
        if (has_sender_friend_info) {
            envelope["origin"]["user_info"] = buildUserInfoJson(sender_friend_info.user_info);
            envelope["origin"]["friend_info"] = buildFriendInfoJson(sender_friend_info);
            envelope["origin"]["agentnet"] = buildAgentNetJson(mCarrier.get(), sender_user_id.c_str());
        } else {
            envelope["origin"]["user_info"]["userid"] = sender_user_id;
            envelope["origin"]["user_info"]["name"] = sender_nickname;
            envelope["origin"]["user_info"]["description"] = "";
            envelope["origin"]["user_info"]["has_avatar"] = 0;
            envelope["origin"]["user_info"]["gender"] = "";
            envelope["origin"]["user_info"]["phone"] = "";
            envelope["origin"]["user_info"]["email"] = "";
            envelope["origin"]["user_info"]["region"] = "";

            int status_code = 1;
            if (sender_member_info.get() != nullptr) {
                status_code = sender_member_info->mStatus;
            }
            ElaConnectionStatus status = status_code == 0
                                         ? ElaConnectionStatus_Connected
                                         : ElaConnectionStatus_Disconnected;
            envelope["origin"]["friend_info"]["label"] = "";
            envelope["origin"]["friend_info"]["status"] = status_code;
            envelope["origin"]["friend_info"]["status_text"] = connectionStatusToString(status);
            envelope["origin"]["friend_info"]["presence"] = static_cast<int>(ElaPresenceStatus_None);
            envelope["origin"]["friend_info"]["presence_text"] = presenceStatusToString(ElaPresenceStatus_None);
        }
        envelope["message"]["text"] = body_text;
        if (is_file) {
            // Agents keep the group envelope (group + real sender) and get the
            // bytes inside it as base64 when they fit; agents that do not know
            // data_base64 ignore it and still see the description.
            envelope["message"]["file"]["filename"] = metaString(file_meta, "filename", "file");
            envelope["message"]["file"]["contentType"] =
                    metaString(file_meta, "contentType", "application/octet-stream");
            envelope["message"]["file"]["size"] =
                    static_cast<unsigned long long>(message->mMsg->size() - 4 - file_meta_len);
        }
        envelope["message"]["timestamp"] = static_cast<long long>(message->mSendTimeStamp);
        envelope["render"]["plain"] = plain_text;
        if (is_file) {
            const size_t body_offset = 4 + file_meta_len;
            const size_t body_len = message->mMsg->size() - body_offset;
            // Cheap pre-check before encoding: base64 is 4/3 of the bytes.
            if (((body_len + 2) / 3) * 4 + 4096 < kFileMessageMaxLen) {
                envelope["message"]["file"]["data_base64"] =
                        base64Encode(message->mMsg->data() + body_offset, body_len);
                std::string with_data = std::string(kAgentOutboundPrefix) + dumpJsonForTransport(envelope);
                if (with_data.size() <= kFileMessageMaxLen) {
                    return with_data;
                }
                envelope["message"]["file"].erase("data_base64");
            }
        }
        return std::string(kAgentOutboundPrefix) + dumpJsonForTransport(envelope);
    }

    bool CarrierRobot::isGroupCreator(const std::string &friend_id) {
        return mCreaterFriendId.get() != nullptr
               && mCreaterFriendId->compare(friend_id) == 0;
    }

    void CarrierRobot::helpCmd(const std::vector<std::string> &args, const std::string &message) {
        if (args.size() >= 2) {
            const std::string friend_id = args[1];
            int ela_ret = ela_send_friend_message_noreceipt(mCarrier.get(), friend_id.c_str(),
                                                  message.data(), message.size());
            if (ela_ret != 0) {
                Log::I(Log::TAG,
                       "helpCmd .c_str(): %s errno:(0x%x)",
                       message.c_str(), ela_get_error());
            }
        }

    }

    void CarrierRobot::listCmd(const std::vector<std::string> &args) {
        if (args.size() >= 2) {
            const std::string friend_id = args[1];
            std::shared_ptr<std::vector<std::shared_ptr<MemberInfo>>> memberlist = mDatabaseProxy->getFriendList();
            std::string ret_msg_str = "";
            for (int i = 0; i < memberlist->size(); i++) {
                std::shared_ptr<MemberInfo> memberInfo = memberlist->at(i);
                if (memberInfo.get() == nullptr) {
                    continue;
                }
                memberInfo->Lock();
                ret_msg_str += std::string(
                        std::to_string(memberInfo->mIndex) + ": " +
                        (*memberInfo->mNickName.get()) + " " +
                        std::string(memberInfo->mStatus == 0 ? "online" : "offline") + "\n");
                memberInfo->UnLock();//同名还没处理
            }

            int ela_ret = ela_send_friend_message_noreceipt(mCarrier.get(), friend_id.c_str(),
                                                  ret_msg_str.data(),
                                                  ret_msg_str.size());
            if (ela_ret != 0) {
                Log::I(Log::TAG,
                       "listCmd .c_str(): %s errno:(0x%x)",
                       ret_msg_str.c_str(), ela_get_error());
            }
        }

    }

    int CarrierRobot::getGroupStatus() {
        return mStatus;
    }

    std::shared_ptr<std::string> CarrierRobot::getGroupNickName() {
        std::shared_ptr<std::string> nick_name = mDatabaseProxy->getGroupNickName();
        return nick_name;
    }

    void CarrierRobot::updateNickNameCmd(const std::vector<std::string> &args) {
        MUTEX_LOCKER locker_sync_data(_mReplyMessage);
        if (args.size() >= 3) {
            const std::string friend_id = args[2];
            if ((*mCreaterFriendId.get()).compare((friend_id)) == 0) {
                const std::string nick_name = args[1];
                mDatabaseProxy->updateGroupNickName(std::make_shared<std::string>(nick_name));
                json msg;
                msg["cmd"] =Command_UpdateNickName;
                msg["friendid"] = mAddress.c_str();
                msg["nickname"] = nick_name.c_str();
                sendMsgForManager(dumpJsonForTransport(msg));
            }
        }
    }

    void CarrierRobot::deleteGroupCmd(const std::vector<std::string> &args) {
        MUTEX_LOCKER locker_sync_data(_mReplyMessage);
        if (args.size() >= 2) {
            const std::string friend_id = args[1];
            if ((*mCreaterFriendId.get()).compare((friend_id)) == 0) {
                mStatus = -1;
                json msg;
                msg["cmd"] = Command_UpdateStatus;
                msg["friendid"] = mAddress.c_str();
                msg["my_socket_fd"] = mMySocketFd;
                msg["status"] = mStatus;
                sendMsgForManager(dumpJsonForTransport(msg));
                //好友解除关系
                std::shared_ptr<std::vector<std::shared_ptr<MemberInfo>>> memberlist = mDatabaseProxy->getFriendList();
                for (int i = 0; i < memberlist->size(); i++) {
                    std::shared_ptr<MemberInfo> memberInfo = memberlist->at(i);
                    if (memberInfo.get() == nullptr) {
                        continue;
                    }
                    memberInfo->Lock();
                    int ela_ret = ela_remove_friend(mCarrier.get(),
                                                    memberInfo->mFriendid.get()->c_str());
                    if (ela_ret != 0) {
                        Log::I(Log::TAG,
                               "deleteGroupCmd can't delete this user: %s errno:(0x%x)",
                               memberInfo->mFriendid.get()->c_str(), ela_get_error());
                    }
                    memberInfo->UnLock();
                }
            }
        }
    }

    void CarrierRobot::blockFriendCmd(const std::vector<std::string> &args) {
        if (args.size() >= 3) {
            const std::string friend_id = args[2];
            if ((*mCreaterFriendId.get()).compare((friend_id)) == 0) {
                std::string del_userindex = args[1];
                int user_num = std::atoi(del_userindex.c_str());
                std::shared_ptr<MemberInfo> memberInfo = mDatabaseProxy->getMemberInfo(user_num);
                char msg_str[256];
                if (memberInfo.get() != nullptr) {
                    memberInfo->Lock();
                    std::string del_friend_id = *memberInfo->mFriendid.get();
                    memberInfo->UnLock();
                    mDatabaseProxy->addBlockMember(memberInfo);
                    mDatabaseProxy->removeMember(del_friend_id);

                    int ela_ret = ela_remove_friend(mCarrier.get(),
                                                    memberInfo->mFriendid.get()->c_str());
                    sprintf(msg_str, "%s has been blocked!",
                            memberInfo->mNickName.get()->c_str());
                    if (ela_ret != 0) {
                        Log::I(Log::TAG,
                               "delCmd can't delete this user: %s errno:(0x%x)",
                               memberInfo->mFriendid.get()->c_str(), ela_get_error());
                    }
                } else {
                    sprintf(msg_str, "num %s member not exist!", del_userindex.c_str());
                }
                ela_send_friend_message_noreceipt(mCarrier.get(), friend_id.c_str(),
                                        msg_str, strlen(msg_str));
                json msg;
                msg["cmd"] = Command_UpdateMemberCount;
                msg["friendid"] = mAddress.c_str();
                msg["membercount"] = getFriendList()->size();
                sendMsgForManager(dumpJsonForTransport(msg));
            }
        }
    }

    void CarrierRobot::delCmd(const std::vector<std::string> &args) {
        if (args.size() >= 3) {
            const std::string friend_id = args[2];
            if ((*mCreaterFriendId.get()).compare((friend_id)) == 0) {
                std::string del_userindex = args[1];
                int user_num = std::atoi(del_userindex.c_str());
                std::shared_ptr<MemberInfo> memberInfo = mDatabaseProxy->getMemberInfo(user_num);
                char msg_str[256];
                if (memberInfo.get() != nullptr) {
                    memberInfo->Lock();
                    std::string del_friend_id = *memberInfo->mFriendid.get();
                    memberInfo->UnLock();
                    mDatabaseProxy->removeMember(del_friend_id);
                    int ela_ret = ela_remove_friend(mCarrier.get(),
                                                    memberInfo->mFriendid.get()->c_str());
                    sprintf(msg_str, "%s has been kicked out!",
                            memberInfo->mNickName.get()->c_str());
                    if (ela_ret != 0) {
                        Log::I(Log::TAG,
                               "delCmd can't delete this user: %s errno:(0x%x)",
                               memberInfo->mFriendid.get()->c_str(), ela_get_error());
                    }
                } else {
                    sprintf(msg_str, "num %s member not exist!", del_userindex.c_str());
                }

                ela_send_friend_message_noreceipt(mCarrier.get(), friend_id.c_str(),
                                        msg_str, strlen(msg_str));

                json msg;
                msg["cmd"] = Command_UpdateMemberCount;
                msg["friendid"] = mAddress.c_str();
                msg["membercount"] = getFriendList()->size();
                sendMsgForManager(dumpJsonForTransport(msg));
            }
        }
    }

    void CarrierRobot::registerCarrierCallBack(const std::function<void (const std::string)> &carrierCallBack) {
        mCarrierCallBack = carrierCallBack;
    }

    void  CarrierRobot::sendMsgForManager(std::string msg) {
        if (mCarrierCallBack != nullptr) {
            mCarrierCallBack(msg);
        }
    }
    void CarrierRobot::showAddressCmd(const std::vector<std::string> &args) {
        if (args.size() >= 2) {
            std::string address;
            this->getAddress(address);

            const std::string friend_id = args[1];
            int ela_ret = ela_send_friend_message_noreceipt(mCarrier.get(), friend_id.c_str(),
                                                  address.data(), address.size());
            if (ela_ret != 0) {
                Log::I(Log::TAG,
                       "showAddressCmd .c_str(): %s errno:(0x%x)",
                       address.c_str(), ela_get_error());
            }
        }

    }

    void CarrierRobot::agentCmd(const std::vector<std::string> &args) {
        if (args.size() < 2) {
            return;
        }
        const std::string friend_id = args.back();

        if (args.size() == 2) {
            sendCommandResponse(friend_id,
                                "Usage: /agent <address> | /agent add <address> | /agent del <userid> | /agent list");
            return;
        }

        const std::string action_or_address = args[1];

        if (action_or_address == "list"
            || action_or_address == "ls") {
            auto agent_user_ids = mDatabaseProxy->getAgentUserIdList();
            if (agent_user_ids.get() == nullptr || agent_user_ids->empty()) {
                sendCommandResponse(friend_id, "No agent registered for this group.");
                return;
            }

            std::string result = "Agent list:\n";
            for (int i = 0; i < agent_user_ids->size(); i++) {
                const std::string user_id = *agent_user_ids->at(i).get();
                std::shared_ptr<std::string> address = mDatabaseProxy->getAgentAddress(user_id);
                result += user_id;
                result += " ";
                result += (address.get() != nullptr) ? *address.get() : "";
                result += "\n";
            }
            sendCommandResponse(friend_id, result);
            return;
        }

        if (action_or_address == "del"
            || action_or_address == "remove"
            || action_or_address == "rm") {
            if (args.size() < 4) {
                sendCommandResponse(friend_id, "Usage: /agent del <userid>");
                return;
            }
            if (!isGroupCreator(friend_id)) {
                sendCommandResponse(friend_id, "Only group creator can remove agent.");
                return;
            }

            const std::string agent_user_id = args[2];
            std::string remove_error;
            if (!removeAgentByUserId(agent_user_id, remove_error)) {
                sendCommandResponse(friend_id,
                                    std::string("Failed to remove agent: ") + remove_error);
                return;
            }

            sendCommandResponse(friend_id, std::string("Agent removed: ") + agent_user_id);
            return;
        }

        std::string agent_address = action_or_address;
        if (action_or_address == "add") {
            if (args.size() < 4) {
                sendCommandResponse(friend_id, "Usage: /agent add <address>");
                return;
            }
            agent_address = args[2];
        }

        if (!isGroupCreator(friend_id)) {
            sendCommandResponse(friend_id, "Only group creator can add agent.");
            return;
        }

        std::string error_message;
        if (!requestAgentFriend(agent_address, error_message)) {
            sendCommandResponse(friend_id,
                                std::string("Unable to add agent: ") + error_message);
            return;
        }

        std::string agent_user_id;
        GetCarrierUsrIdByAddress(agent_address, agent_user_id);
        sendCommandResponse(friend_id,
                            std::string("Agent registered: userid=") + agent_user_id
                            + " address=" + agent_address
                            + ". You only need this once unless agent is removed.");
    }

    // The signer list is the payload the group key signs for the group's
    // beagles.eth record. Sorted by byte order and comma-joined, so the same
    // set always produces the same bytes regardless of insertion order.
    std::string CarrierRobot::buildSignerProofPayload(std::time_t timestamp) {
        auto signers = mDatabaseProxy->getSignerUserIdList();
        std::vector<std::string> ids;
        if (signers.get() != nullptr) {
            for (int i = 0; i < signers->size(); i++) {
                ids.push_back(*signers->at(i).get());
            }
        }
        std::sort(ids.begin(), ids.end());

        std::string joined;
        for (size_t i = 0; i < ids.size(); i++) {
            if (i > 0) {
                joined += ",";
            }
            joined += ids[i];
        }

        std::string group_user_id;
        getUserId(group_user_id);

        return std::string("beagle-group-signers\n") + group_user_id + "\n" + joined + "\n"
               + std::to_string(static_cast<long long>(timestamp));
    }

    std::string CarrierRobot::describeSigners() {
        auto signers = mDatabaseProxy->getSignerUserIdList();
        if (signers.get() == nullptr || signers->empty()) {
            return "No signer registered for this group.";
        }
        std::string result = "Signer list:\n";
        for (int i = 0; i < signers->size(); i++) {
            result += *signers->at(i).get();
            result += "\n";
        }
        return result;
    }

    void CarrierRobot::signerCmd(const std::vector<std::string> &args) {
        if (args.size() < 2) {
            return;
        }
        const std::string friend_id = args.back();

        if (args.size() == 2) {
            sendCommandResponse(friend_id,
                                "Usage: /signer add <userid> | /signer del <userid> | /signer list");
            return;
        }

        const std::string action = args[1];

        if (action == "list" || action == "ls") {
            const std::time_t now = std::time(nullptr);
            sendCommandResponse(friend_id,
                                describeSigners()
                                + "\nProof payload (UNSIGNED, not published):\n"
                                + buildSignerProofPayload(now));
            return;
        }

        if (action != "add" && action != "del" && action != "remove" && action != "rm") {
            sendCommandResponse(friend_id,
                                "Usage: /signer add <userid> | /signer del <userid> | /signer list");
            return;
        }

        if (args.size() < 4) {
            sendCommandResponse(friend_id,
                                std::string("Usage: /signer ") + action + " <userid>");
            return;
        }

        if (!isGroupCreator(friend_id)) {
            sendCommandResponse(friend_id, "Only group creator can change signers.");
            return;
        }

        // A signer is a Carrier userid (base58 public key), never an address:
        // an address carries four extra bytes and would never match a signer
        // the gateway sees.
        const std::string signer_user_id = args[2];
        static const std::string kBase58 =
                "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
        bool valid = signer_user_id.size() >= 43 && signer_user_id.size() <= 45;
        for (size_t i = 0; valid && i < signer_user_id.size(); i++) {
            if (kBase58.find(signer_user_id[i]) == std::string::npos) {
                valid = false;
            }
        }
        if (!valid) {
            sendCommandResponse(friend_id,
                                std::string("Not a Carrier userid: ") + signer_user_id
                                + ". Expected 43-45 base58 chars, not a 52-char address.");
            return;
        }

        if (action == "add") {
            if (!mDatabaseProxy->addSigner(signer_user_id)) {
                sendCommandResponse(friend_id, "Failed to add signer.");
                return;
            }
        } else {
            if (!mDatabaseProxy->hasSigner(signer_user_id)) {
                sendCommandResponse(friend_id,
                                    std::string("Not a signer: ") + signer_user_id);
                return;
            }
            if (!mDatabaseProxy->removeSigner(signer_user_id)) {
                sendCommandResponse(friend_id, "Failed to remove signer.");
                return;
            }
        }

        // TODO(signing): sign this payload with the group's own Carrier key
        // (XEdDSA) and store signers + signersProof "<sigHex>.<ts>" for the
        // group's beagles.eth record. The key is not reachable from the SDK
        // yet, so the payload is reported unsigned and nothing is published.
        const std::time_t now = std::time(nullptr);
        const std::string payload = buildSignerProofPayload(now);
        sendCommandResponse(friend_id,
                            std::string(action == "add" ? "Signer added: " : "Signer removed: ")
                            + signer_user_id + "\n\n" + describeSigners()
                            + "\nProof payload (UNSIGNED, not published):\n" + payload);
    }

}
