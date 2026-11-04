//
// Created by luocf on 2019/6/12.
//

#ifndef CHATROBOT_DB_PROXY_H
#define CHATROBOT_DB_PROXY_H

#include <string>
#include <map>
#include <vector>
#include <mutex>
#include <sqlite3.h>
#include <stdlib.h>
#include <iostream>
#include <memory> // std::unique_ptr
#include <ctime>
#include <regex>
#include "MessageInfo.h"
#include "MemberInfo.h"

namespace chatrobot {
    typedef std::lock_guard<std::mutex> MUTEX_LOCKER;
    typedef bool (*MessageListCallback)(void *context,
                                        std::shared_ptr<std::vector<std::shared_ptr<MessageInfo>>> message_list);

    class DatabaseProxy : std::enable_shared_from_this<DatabaseProxy> {
    public:

        DatabaseProxy();

        virtual ~DatabaseProxy();

        static constexpr const char *TAG = "DatabaseProxy";

        void updateMemberInfo(std::shared_ptr<std::string> friendid,
                              std::shared_ptr<std::string> nickname,
                              int status,
                              std::time_t time_stamp);

        std::shared_ptr<MemberInfo> getMemberInfo(std::shared_ptr<std::string> friendid);
        std::shared_ptr<MemberInfo> getMemberInfo(int index);
        std::shared_ptr<MemberInfo> getBlockMemberInfo(std::shared_ptr<std::string> friendid);
        std::shared_ptr<MemberInfo> getBlockMemberInfo(int index);
        std::shared_ptr<std::string> getGroupNickName();
        void updateGroupNickName(std::shared_ptr<std::string> nick_name);
        void
        addMessgae(std::shared_ptr<std::string> friend_id, std::string message,
                   std::time_t send_time);

        std::shared_ptr<std::vector<std::shared_ptr<MemberInfo>>> getFriendList();

        bool removeMember(std::string friendid);
        bool deleteRemovedListMember(std::string friendid);
        bool inRemovedList(std::string friendid);
        bool startDb(const char *data_dir);
        bool closeDb();
        // A message from friend_id with exactly these bytes stored at or after since (us).
        bool hasMessageSince(const std::string &friend_id, const std::string &message, std::time_t since);
        // True when stored; false when this (sender, post time, length) is already kept.
        bool addExpressBacklog(const std::string &friend_id, const std::string &message,
                               std::time_t post_time, std::time_t recv_time);
        void dedupeExpressBacklog();
        // service_meta[key], set to init_value the first time.
        std::time_t getOrInitMeta(const std::string &key, std::time_t init_value);
        std::shared_ptr<std::vector<std::shared_ptr<MessageInfo>>>
        getMessages(std::shared_ptr<std::string> friend_id, std::time_t send_time, int max_limit);

        void addBlockMember(std::shared_ptr<MemberInfo> memberinfo);
        bool removeBlockMember(std::string friendid);
        bool addAgent(const std::string& user_id, const std::string& address);
        bool removeAgent(const std::string& user_id);
        bool hasAgent(const std::string& user_id);
        std::shared_ptr<std::string> getAgentAddress(const std::string& user_id);
        std::shared_ptr<std::vector<std::shared_ptr<std::string>>> getAgentUserIdList();
        bool addSigner(const std::string& user_id);
        bool removeSigner(const std::string& user_id);
        bool hasSigner(const std::string& user_id);
        std::shared_ptr<std::vector<std::shared_ptr<std::string>>> getSignerUserIdList();
    private:
        std::mutex _SyncedGroupInfo;
        std::mutex _SyncedMemberList;
        std::mutex _SyncedBlockMemberList;
        std::mutex _SyncedMessageList;
        std::mutex _SyncedAgentList;
        std::shared_ptr<std::string> mNickName;
        sqlite3 *mDb;
        std::shared_ptr<std::vector<std::shared_ptr<MemberInfo>>> mBlockMemberList;
        std::shared_ptr<std::vector<std::shared_ptr<MemberInfo>>> mMemberList;
        std::shared_ptr<std::vector<std::shared_ptr<MemberInfo>>> mRemovedMemberList;
        std::shared_ptr<std::vector<std::shared_ptr<MessageInfo>>> mMessageList;
        std::shared_ptr<std::map<std::string, std::string>> mAgentMap;
        std::mutex _SyncedSignerList;
        std::shared_ptr<std::vector<std::string>> mSignerList;
        void syncBlockList();
        void syncMemberList();
        void syncGroupInfo();
        void syncAgentList();
        void syncSignerList();
        static int callback(void *context, int argc, char **argv, char **azColName);

    };

}
#endif //CHATROBOT_DB_PROXY_H
