//
// Created by luocf on 2019/6/13.
//
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <assert.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "CarrierCompat.h"
#include "CarrierConfig.h"

namespace chatrobot {

    CarrierConfig::CarrierConfig() {
        NickName = std::make_shared<std::string>("ChatRobot");
        mLogLevel = ElaLogLevel_Verbose;
        mEnableUdp = true;
        auto node1 = std::make_shared<chatrobot::Node>(
                std::make_shared<std::string>("13.58.208.50"),
                std::make_shared<std::string>("33445"),
                std::make_shared<std::string>(
                        "89vny8MrKdDKs7Uta9RdVmspPjnRMdwMmaiEW27pZ7gh"));
        mBootstrapNodes.push_back(node1);
        auto node2 = std::make_shared<chatrobot::Node>(
                std::make_shared<std::string>("18.216.6.197"),
                std::make_shared<std::string>("33445"),
                std::make_shared<std::string>(
                        "H8sqhRrQuJZ6iLtP2wanxt4LzdNrN2NNFnpPdq1uJ9n2"));
        mBootstrapNodes.push_back(node2);
        auto node3 = std::make_shared<chatrobot::Node>(
                std::make_shared<std::string>("52.83.127.85"),
                std::make_shared<std::string>("33445"),
                std::make_shared<std::string>(
                        "CDkze7mJpSuFAUq6byoLmteyGYMeJ6taXxWoVvDMexWC"));
        mBootstrapNodes.push_back(node3);
        auto node4 = std::make_shared<chatrobot::Node>(
                std::make_shared<std::string>("52.83.127.216"),
                std::make_shared<std::string>("33445"),
                std::make_shared<std::string>(
                        "4sL3ZEriqW7pdoqHSoYXfkc1NMNpiMz7irHMMrMjp9CM"));
        mBootstrapNodes.push_back(node4);
        auto node5 = std::make_shared<chatrobot::Node>(
                std::make_shared<std::string>("52.83.171.135"),
                std::make_shared<std::string>("33445"),
                std::make_shared<std::string>(
                        "5tuHgK1Q4CYf4K5PutsEPK5E3Z7cbtEBdx7LwmdzqXHL"));
        mBootstrapNodes.push_back(node5);
        auto node6 = std::make_shared<chatrobot::Node>(
                std::make_shared<std::string>("52.83.191.228"),
                std::make_shared<std::string>("33445"),
                std::make_shared<std::string>(
                        "3khtxZo89SBScAMaHhTvD68pPHiKxgZT6hTCSZZVgNEm"));
        mBootstrapNodes.push_back(node6);

        auto addNode = [](std::vector<std::shared_ptr<Node>> &list, const char *ip,
                          const char *port, const char *key) {
            list.push_back(std::make_shared<chatrobot::Node>(
                    std::make_shared<std::string>(ip), std::make_shared<std::string>(port),
                    std::make_shared<std::string>(key)));
        };
        // The rest of the iOS app's bootstrap list (bootstrap.json, 2026-10-24):
        // the six above date from 2019 and are not all alive.
        addNode(mBootstrapNodes, "18.216.102.47", "33445", "G5z8MqiNDFTadFUPfMdYsYtkUDbX5mNCMVHMZtsCnFeb");
        addNode(mBootstrapNodes, "54.193.141.205", "33445", "7TfZWZNV8vnBxxWzJXuvKgX2QyKkLpg2oXx3LQ5tg8LW");
        addNode(mBootstrapNodes, "52.74.215.181", "33445", "Xv6d34WaUw9bPn7YihzVAFw7D2igbQJZ3jwmzzfYVFV");
        addNode(mBootstrapNodes, "52.63.19.190", "33445", "EeNenbyS4sx3qtu82esT1V1NMe9dZib5LyQmYGM6fboK");
        addNode(mBootstrapNodes, "52.57.248.163", "33445", "CfJLve8FNQPQJ9xYQ8oEVkPxeAPCN7iSdhnYFkWmWgLn");
        addNode(mBootstrapNodes, "35.179.41.220", "33445", "6u2vKadPa9wqDf531QaZy7FJN3c7Wzntm7dKXxXqvyNB");
        addNode(mBootstrapNodes, "43.129.244.17", "33445", "8qoXec3GGYGvMrnim4mNwsCUZeygjMizXgFjTEuFPA9u");

        // Same Express nodes, same order, as the apps (lens first: senders on
        // older cores store only there). The core pulls and deletes from every node.
        addNode(mExpressNodes, "lens.beagle.chat", "443", "ECbs4GxwGzxGerNkmqDJFibEmevu8jAXqAZtikccvD95");
        addNode(mExpressNodes, "express.beagle.chat", "443", "fhZcWat6TPhKSnnnLtb1w7hsi5xoz9fkh7jJ8j2HrUz");

    }
}