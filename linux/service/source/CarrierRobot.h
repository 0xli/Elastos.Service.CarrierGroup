//
// Created by luocf on 2019/6/13.
//

#ifndef CHATROBOT_CARRIERROBOT_H
#define CHATROBOT_CARRIERROBOT_H

#include <stdlib.h>
#include <functional>
#include <string>
#include <iostream>
#include <thread>
#include <future>
#include <queue>
#include <memory> // std::unique_ptr
#include <ctime>
#include <thread>
#include <regex>
#include "CarrierCompat.h"
#include <carrier_session.h>
#include <carrier_session_deprecated.h>
#include "CarrierCompat.h"
#include "DataBase/DatabaseProxy.h"
#include <CarrierConfig.h>

namespace chatrobot {
    static const char *TAG = "CarrierRobot";
    class CarrierRobot:std::enable_shared_from_this<CarrierRobot> {
    public:
        class Factory {
        public:
            static int SetLocalDataDir(const std::string& dir);
            static std::shared_ptr<CarrierRobot> Create();

        private:
            static std::string sLocalDataDir;
            friend class CarrierRobot;
        };
        ~CarrierRobot();
        int start(const char* data_dir, int service_id, int my_socket_fd);
        void stop();
        int getAddress(std::string& address);
        int acceptFriend(std::shared_ptr<std::string>friendid);
        int getUserId(std::string& userid);
        static void OnCarrierConnection(ElaCarrier *carrier,
                                        ElaConnectionStatus status, void *context);
        static void OnCarrierFriendRequest(ElaCarrier *carrier, const char *friendid,
                                           const ElaUserInfo *info,
                                           const char *hello, void *context);
        static void OnCarrierFriendInfoChanged(ElaCarrier *carrier, const char *friendid,
                                               const ElaFriendInfo *info, void *context);
        static void OnCarrierFriendConnection(ElaCarrier *carrier,const char *friendid,
                                              ElaConnectionStatus status, void *context);

        static void OnCarrierFriendMessage(ElaCarrier *carrier, const char *from,
                                           const void *msg, size_t len,
                                           int64_t timestamp, bool offline,
                                           void *context);

        static int GetCarrierUsrIdByAddress(const std::string& address, std::string& usrId);
        void runCarrierInner();
        void runCarrier();
        bool inRemovedList(std::shared_ptr<std::string> friendid);
        void showAddressCmd(const std::vector<std::string> &args);
        void updateMemberInfo(std::shared_ptr<std::string> friendid, std::shared_ptr<std::string> nickname,
                              ElaConnectionStatus status);
        // offline: pulled from Express; post_time: the core's timestamp for it.
        void addMessgae(std::shared_ptr<std::string> friend_id, std::shared_ptr<std::string> message, std::time_t send_time,
                        bool offline = false, int64_t post_time = 0);
        // False when an Express copy must not be stored/relayed (duplicate or backlog).
        bool acceptOfflineCopy(const std::string &friend_id, const std::string &stored_form,
                               std::time_t recv_time, int64_t post_time);
        std::shared_ptr<std::vector<std::shared_ptr<MemberInfo>>>getFriendList();
        void helpCmd(const std::vector<std::string> &args, const std::string& message);
        void listCmd(const std::vector<std::string> &args);
        void delCmd(const std::vector<std::string> &args);
        void blockFriendCmd(const std::vector<std::string> &args);
        void updateNickNameCmd(const std::vector<std::string> &args);
        void deleteGroupCmd(const std::vector<std::string> &args);
        void agentCmd(const std::vector<std::string> &args);
        void signerCmd(const std::vector<std::string> &args);
        bool isGroupCreator(const std::string& friend_id);
        bool addAgentByAddress(const std::string& address, std::string& error_message);
        bool removeAgentByUserId(const std::string& user_id, std::string& error_message);
        std::shared_ptr<std::vector<std::shared_ptr<std::string>>> getAgentUserIdList();
        std::shared_ptr<std::string> getAgentAddressByUserId(const std::string& user_id);
        std::shared_ptr<std::string> getGroupNickName();
        int getGroupStatus();
        void registerCarrierCallBack(const std::function<void(const std::string)> &carrierCallBack);
    private:
        std::shared_ptr<std::string> resolveGroupDisplayName(int service_id);
        std::shared_ptr<std::string> resolveMemberDisplayName(const char *friendid, const char *display_name);
        int applySelfDisplayName(int service_id);
        bool requestAgentFriend(const std::string &address, std::string &error_message);
        std::string buildSignerProofPayload(std::time_t timestamp);
        std::string describeSigners();
        void sendCommandResponse(const std::string &friend_id, const std::string &message);
        std::string normalizeIncomingMessageForStorage(const std::string& friend_id,
                                                       const std::string& message);
        std::string buildOutboundMessageForRecipient(const std::string& recipient_user_id,
                                                     std::shared_ptr<MessageInfo> message);
        std::string normalizeAgentStatusEnvelope(const std::string& friend_id,
                                                 const std::string& message);
        bool relayAgentStatusToMembers(const std::string& agent_user_id,
                                       const std::string& status_message);

        static CarrierRobot* instance;

        explicit CarrierRobot();
        std::time_t getTimeStamp();
        std::string convertDatetimeToString(std::time_t time);
        bool relayMessages();
        std::shared_ptr<std::regex> mMsgReg;
        std::mutex _mReplyMessage;
        std::thread mCarrieryThread;
        std::shared_ptr<std::string> mLocalPath;
        std::shared_ptr<std::string> mCreaterFriendId;
        std::unique_ptr<ElaCarrier, std::function<void(ElaCarrier*)>> mCarrier;
        std::shared_ptr<CarrierConfig> mCarrierConfig;
        std::shared_ptr<DatabaseProxy> mDatabaseProxy;
        int mStatus;
        int mMySocketFd;
        // First time this group pulled Express (us). Parked before it = backlog.
        std::time_t mExpressSince = 0;
        std::function <void(const std::string)> mCarrierCallBack;
        std::string mAddress;
        void sendMsgForManager(std::string msg);
    };
}

#endif //CHATROBOT_CARRIERROBOT_H
