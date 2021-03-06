/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include <boost/scoped_ptr.hpp>
#include <boost/thread.hpp>

#include "mongo/base/status.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/member_heartbeat_data.h"
#include "mongo/db/repl/network_interface_mock.h"
#include "mongo/db/repl/replica_set_config.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/db/repl/freshness_checker.h"
#include "mongo/stdx/functional.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace repl {
namespace {

    typedef ReplicationExecutor::RemoteCommandRequest RemoteCommandRequest;

    bool stringContains(const std::string &haystack, const std::string& needle) {
        return haystack.find(needle) != std::string::npos;
    }

    class FreshnessCheckerTest : public mongo::unittest::Test {
    public:
        FreshnessCheckerTest();
        void freshnessCheckerRunner(const ReplicationExecutor::CallbackData& data,
                                    FreshnessChecker* checker,
                                    const ReplicationExecutor::EventHandle& evh,
                                    const OpTime& lastOpTimeApplied, 
                                    const ReplicaSetConfig& currentConfig,
                                    int selfIndex,
                                    const std::vector<HostAndPort>& hosts);
    protected:
        int64_t countLogLinesContaining(const std::string& needle) {
            return std::count_if(getCapturedLogMessages().begin(),
                                 getCapturedLogMessages().end(),
                                 stdx::bind(stringContains,
                                            stdx::placeholders::_1,
                                            needle));
        }

        NetworkInterfaceMockWithMap* _net;
        boost::scoped_ptr<ReplicationExecutor> _executor;
        boost::scoped_ptr<boost::thread> _executorThread;
        Status _lastStatus;

    private:
        void setUp();
        void tearDown();
    };

    FreshnessCheckerTest::FreshnessCheckerTest() : _lastStatus(Status::OK()) {}

    void FreshnessCheckerTest::setUp() {
        _net = new NetworkInterfaceMockWithMap;
        _executor.reset(new ReplicationExecutor(_net, 1 /* prng seed */));
        _executorThread.reset(new boost::thread(stdx::bind(&ReplicationExecutor::run,
                                                           _executor.get())));
    }

    void FreshnessCheckerTest::tearDown() {
        _net->unblockAll();
        _executor->shutdown();
        _executorThread->join();
    }

    ReplicaSetConfig assertMakeRSConfig(const BSONObj& configBson) {
        ReplicaSetConfig config;
        ASSERT_OK(config.initialize(configBson));
        ASSERT_OK(config.validate());
        return config;
    }

    const BSONObj makeFreshRequest(const ReplicaSetConfig& rsConfig, 
                                   OpTime lastOpTimeApplied, 
                                   int selfIndex) {
        const MemberConfig& myConfig = rsConfig.getMemberAt(selfIndex);
        return BSON("replSetFresh" << 1 <<
                    "set" << rsConfig.getReplSetName() <<
                    "opTime" << Date_t(lastOpTimeApplied.asDate()) <<
                    "who" << myConfig.getHostAndPort().toString() <<
                    "cfgver" << rsConfig.getConfigVersion() <<
                    "id" << myConfig.getId());
    }

    // This is necessary because the run method must be scheduled in the Replication Executor
    // for correct concurrency operation.
    void FreshnessCheckerTest::freshnessCheckerRunner(
        const ReplicationExecutor::CallbackData& data,
        FreshnessChecker* checker,
        const ReplicationExecutor::EventHandle& evh,
        const OpTime& lastOpTimeApplied, 
        const ReplicaSetConfig& currentConfig,
        int selfIndex,
        const std::vector<HostAndPort>& hosts) {
        invariant(data.status.isOK());
        _lastStatus = checker->start(data.executor, 
                                     evh, 
                                     lastOpTimeApplied, 
                                     currentConfig, 
                                     selfIndex, 
                                     hosts);
    }

    TEST_F(FreshnessCheckerTest, OneNode) {
        // Only one node in the config.  We are freshest and not tied.
        ReplicaSetConfig config = assertMakeRSConfig(
                BSON("_id" << "rs0" <<
                     "version" << 1 <<
                     "members" << BSON_ARRAY(
                             BSON("_id" << 1 << "host" << "h1"))));
        
        StatusWith<ReplicationExecutor::EventHandle> evh = _executor->makeEvent();
        ASSERT_OK(evh.getStatus());

        Date_t now(0);
        std::vector<HostAndPort> hosts;
        hosts.push_back(config.getMemberAt(0).getHostAndPort());

        FreshnessChecker checker;
        
        StatusWith<ReplicationExecutor::CallbackHandle> cbh = 
            _executor->scheduleWork(
                stdx::bind(&FreshnessCheckerTest::freshnessCheckerRunner,
                           this,
                           stdx::placeholders::_1,
                           &checker,
                           evh.getValue(),
                           OpTime(0,0),
                           config,
                           0,
                           hosts));
        ASSERT_OK(cbh.getStatus());
        _executor->wait(cbh.getValue());

        ASSERT_OK(_lastStatus);

        _executor->waitForEvent(evh.getValue());

        bool weAreFreshest(false);
        bool tied(false);
        checker.getResults(&weAreFreshest, &tied);
        ASSERT_TRUE(weAreFreshest);
        ASSERT_FALSE(tied);
        
    }

    TEST_F(FreshnessCheckerTest, TwoNodes) {
        // Two nodes, we are node h1.  We are freshest, but we tie with h2.
        ReplicaSetConfig config = assertMakeRSConfig(
                BSON("_id" << "rs0" <<
                     "version" << 1 <<
                     "members" << BSON_ARRAY(
                         BSON("_id" << 1 << "host" << "h0") <<
                         BSON("_id" << 2 << "host" << "h1"))));
        
        StatusWith<ReplicationExecutor::EventHandle> evh = _executor->makeEvent();
        ASSERT_OK(evh.getStatus());

        Date_t now(0);
        std::vector<HostAndPort> hosts;
        hosts.push_back(config.getMemberAt(0).getHostAndPort());
        hosts.push_back(config.getMemberAt(1).getHostAndPort());

        const BSONObj freshRequest = makeFreshRequest(config, OpTime(0,0), 0);

        _net->addResponse(RemoteCommandRequest(HostAndPort("h1"),
                                               "admin",
                                               freshRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1 <<
                                                   "id" << 2 <<
                                                   "set" << "rs0" <<
                                                   "who" << "h1" <<
                                                   "cfgver" << 1 <<
                                                   "opTime" << Date_t(OpTime(0,0).asDate()))));

        FreshnessChecker checker;
        StatusWith<ReplicationExecutor::CallbackHandle> cbh = 
            _executor->scheduleWork(
                stdx::bind(&FreshnessCheckerTest::freshnessCheckerRunner,
                           this,
                           stdx::placeholders::_1,
                           &checker,
                           evh.getValue(),
                           OpTime(0,0),
                           config,
                           0,
                           hosts));
        ASSERT_OK(cbh.getStatus());
        _executor->wait(cbh.getValue());

        ASSERT_OK(_lastStatus);

        _executor->waitForEvent(evh.getValue());

        bool weAreFreshest(false);
        bool tied(false);
        checker.getResults(&weAreFreshest, &tied);
        ASSERT_TRUE(weAreFreshest);
        ASSERT_TRUE(tied);
        
    }


    TEST_F(FreshnessCheckerTest, ShuttingDown) {
        // Two nodes, we are node h1.  Shutdown happens while we're scheduling remote commands.
        ReplicaSetConfig config = assertMakeRSConfig(
                BSON("_id" << "rs0" <<
                     "version" << 1 <<
                     "members" << BSON_ARRAY(
                         BSON("_id" << 1 << "host" << "h0") <<
                         BSON("_id" << 2 << "host" << "h1"))));
        
        StatusWith<ReplicationExecutor::EventHandle> evh = _executor->makeEvent();
        ASSERT_OK(evh.getStatus());

        Date_t now(0);
        std::vector<HostAndPort> hosts;
        hosts.push_back(config.getMemberAt(0).getHostAndPort());
        hosts.push_back(config.getMemberAt(1).getHostAndPort());

        const BSONObj freshRequest = makeFreshRequest(config, OpTime(0,0), 0);
        _net->addResponse(RemoteCommandRequest(HostAndPort("h1"),
                                               "admin",
                                               freshRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1 <<
                                                   "id" << 2 <<
                                                   "set" << "rs0" <<
                                                   "who" << "h1" <<
                                                   "cfgver" << 1 <<
                                                   "opTime" << Date_t(OpTime(0,0).asDate()))),
                          true /* isBlocked */);

        FreshnessChecker checker;
        StatusWith<ReplicationExecutor::CallbackHandle> cbh = 
            _executor->scheduleWork(
                stdx::bind(&FreshnessCheckerTest::freshnessCheckerRunner,
                           this,
                           stdx::placeholders::_1,
                           &checker,
                           evh.getValue(),
                           OpTime(0,0),
                           config,
                           0,
                           hosts));
        ASSERT_OK(cbh.getStatus());

        _executor->wait(cbh.getValue());
        ASSERT_OK(_lastStatus);

        _executor->shutdown();
        _net->unblockAll();
        _executor->waitForEvent(evh.getValue());

        bool weAreFreshest(false);
        bool tied(false);
        checker.getResults(&weAreFreshest, &tied);
        // This seems less than ideal, but if we are shutting down, the next phase of election
        // cannot proceed anyway.
        ASSERT_TRUE(weAreFreshest);
        ASSERT_FALSE(tied); 
    }

    TEST_F(FreshnessCheckerTest, ElectNotElectingSelfWeAreNotFreshest) {
        // other responds as fresher than us
        startCapturingLogMessages();
        ReplicaSetConfig config = assertMakeRSConfig(
                BSON("_id" << "rs0" <<
                     "version" << 1 <<
                     "members" << BSON_ARRAY(
                         BSON("_id" << 1 << "host" << "h0") <<
                         BSON("_id" << 2 << "host" << "h1"))));
        
        StatusWith<ReplicationExecutor::EventHandle> evh = _executor->makeEvent();
        ASSERT_OK(evh.getStatus());

        Date_t now(0);
        std::vector<HostAndPort> hosts;
        hosts.push_back(config.getMemberAt(0).getHostAndPort());
        hosts.push_back(config.getMemberAt(1).getHostAndPort());

        const BSONObj freshRequest = makeFreshRequest(config, OpTime(10,0), 0);

        _net->addResponse(RemoteCommandRequest(HostAndPort("h1"),
                                               "admin",
                                               freshRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1 <<
                                                   "id" << 2 <<
                                                   "set" << "rs0" <<
                                                   "who" << "h1" <<
                                                   "cfgver" << 1 <<
                                                   "fresher" << true <<
                                                   "opTime" << Date_t(OpTime(0,0).asDate()))));

        FreshnessChecker checker;
        StatusWith<ReplicationExecutor::CallbackHandle> cbh = 
            _executor->scheduleWork(
                stdx::bind(&FreshnessCheckerTest::freshnessCheckerRunner,
                           this,
                           stdx::placeholders::_1,
                           &checker,
                           evh.getValue(),
                           OpTime(10,0),
                           config,
                           0,
                           hosts));
        ASSERT_OK(cbh.getStatus());
        _executor->wait(cbh.getValue());

        ASSERT_OK(_lastStatus);

        _executor->waitForEvent(evh.getValue());
        stopCapturingLogMessages();

        bool weAreFreshest(true);
        bool tied(true);
        checker.getResults(&weAreFreshest, &tied);
        ASSERT_FALSE(weAreFreshest);
        ASSERT_FALSE(tied);
        ASSERT_EQUALS(1, countLogLinesContaining("not electing self, we are not freshest"));
    }

    TEST_F(FreshnessCheckerTest, ElectNotElectingSelfWeAreNotFreshestOpTime) {
        // other responds with a later optime than ours
        startCapturingLogMessages();
        ReplicaSetConfig config = assertMakeRSConfig(
                BSON("_id" << "rs0" <<
                     "version" << 1 <<
                     "members" << BSON_ARRAY(
                         BSON("_id" << 1 << "host" << "h0") <<
                         BSON("_id" << 2 << "host" << "h1"))));
        
        StatusWith<ReplicationExecutor::EventHandle> evh = _executor->makeEvent();
        ASSERT_OK(evh.getStatus());

        Date_t now(0);
        std::vector<HostAndPort> hosts;
        hosts.push_back(config.getMemberAt(0).getHostAndPort());
        hosts.push_back(config.getMemberAt(1).getHostAndPort());

        const BSONObj freshRequest = makeFreshRequest(config, OpTime(0,0), 0);

        _net->addResponse(RemoteCommandRequest(HostAndPort("h1"),
                                               "admin",
                                               freshRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1 <<
                                                   "id" << 2 <<
                                                   "set" << "rs0" <<
                                                   "who" << "h1" <<
                                                   "cfgver" << 1 <<
                                                   "fresher" << true <<
                                                   "opTime" << Date_t(OpTime(10,0).asDate()))));

        FreshnessChecker checker;
        StatusWith<ReplicationExecutor::CallbackHandle> cbh = 
            _executor->scheduleWork(
                stdx::bind(&FreshnessCheckerTest::freshnessCheckerRunner,
                           this,
                           stdx::placeholders::_1,
                           &checker,
                           evh.getValue(),
                           OpTime(0,0),
                           config,
                           0,
                           hosts));
        ASSERT_OK(cbh.getStatus());
        _executor->wait(cbh.getValue());

        ASSERT_OK(_lastStatus);

        _executor->waitForEvent(evh.getValue());
        stopCapturingLogMessages();

        bool weAreFreshest(true);
        bool tied(true);
        checker.getResults(&weAreFreshest, &tied);
        ASSERT_FALSE(weAreFreshest);
        ASSERT_FALSE(tied);
        ASSERT_EQUALS(1, countLogLinesContaining("not electing self, we are not freshest"));
    }

    TEST_F(FreshnessCheckerTest, ElectWrongTypeInFreshnessResponse) {
        // other responds with "opTime" field of non-Date value, causing not freshest
        startCapturingLogMessages();
        ReplicaSetConfig config = assertMakeRSConfig(
                BSON("_id" << "rs0" <<
                     "version" << 1 <<
                     "members" << BSON_ARRAY(
                         BSON("_id" << 1 << "host" << "h0") <<
                         BSON("_id" << 2 << "host" << "h1"))));
        
        StatusWith<ReplicationExecutor::EventHandle> evh = _executor->makeEvent();
        ASSERT_OK(evh.getStatus());

        Date_t now(0);
        std::vector<HostAndPort> hosts;
        hosts.push_back(config.getMemberAt(0).getHostAndPort());
        hosts.push_back(config.getMemberAt(1).getHostAndPort());

        const BSONObj freshRequest = makeFreshRequest(config, OpTime(10,0), 0);

        _net->addResponse(RemoteCommandRequest(HostAndPort("h1"),
                                               "admin",
                                               freshRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1 <<
                                                   "id" << 2 <<
                                                   "set" << "rs0" <<
                                                   "who" << "h1" <<
                                                   "cfgver" << 1 <<
                                                   "opTime" << 3)));

        FreshnessChecker checker;
        StatusWith<ReplicationExecutor::CallbackHandle> cbh = 
            _executor->scheduleWork(
                stdx::bind(&FreshnessCheckerTest::freshnessCheckerRunner,
                           this,
                           stdx::placeholders::_1,
                           &checker,
                           evh.getValue(),
                           OpTime(10,0),
                           config,
                           0,
                           hosts));
        ASSERT_OK(cbh.getStatus());
        _executor->wait(cbh.getValue());

        ASSERT_OK(_lastStatus);

        _executor->waitForEvent(evh.getValue());
        stopCapturingLogMessages();

        bool weAreFreshest(true);
        bool tied(true);
        checker.getResults(&weAreFreshest, &tied);
        ASSERT_FALSE(weAreFreshest);
        ASSERT_FALSE(tied);
        ASSERT_EQUALS(1, countLogLinesContaining("wrong type for opTime argument in replSetFresh "
                                                 "response: NumberInt32"));
    }

    TEST_F(FreshnessCheckerTest, ElectVetoed) {
        // other responds with veto
        startCapturingLogMessages();
        ReplicaSetConfig config = assertMakeRSConfig(
                BSON("_id" << "rs0" <<
                     "version" << 1 <<
                     "members" << BSON_ARRAY(
                         BSON("_id" << 1 << "host" << "h0") <<
                         BSON("_id" << 2 << "host" << "h1"))));
        
        StatusWith<ReplicationExecutor::EventHandle> evh = _executor->makeEvent();
        ASSERT_OK(evh.getStatus());

        Date_t now(0);
        std::vector<HostAndPort> hosts;
        hosts.push_back(config.getMemberAt(0).getHostAndPort());
        hosts.push_back(config.getMemberAt(1).getHostAndPort());

        const BSONObj freshRequest = makeFreshRequest(config, OpTime(10,0), 0);

        _net->addResponse(RemoteCommandRequest(HostAndPort("h1"),
                                               "admin",
                                               freshRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1 <<
                                                   "id" << 2 <<
                                                   "set" << "rs0" <<
                                                   "who" << "h1" <<
                                                   "cfgver" << 1 <<
                                                   "veto" << true <<
                                                   "errmsg" << "I'd rather you didn't" <<
                                                   "opTime" << Date_t(OpTime(0,0).asDate()))));

        FreshnessChecker checker;
        StatusWith<ReplicationExecutor::CallbackHandle> cbh = 
            _executor->scheduleWork(
                stdx::bind(&FreshnessCheckerTest::freshnessCheckerRunner,
                           this,
                           stdx::placeholders::_1,
                           &checker,
                           evh.getValue(),
                           OpTime(10,0),
                           config,
                           0,
                           hosts));
        ASSERT_OK(cbh.getStatus());
        _executor->wait(cbh.getValue());

        ASSERT_OK(_lastStatus);

        _executor->waitForEvent(evh.getValue());
        stopCapturingLogMessages();

        bool weAreFreshest(true);
        bool tied(true);
        checker.getResults(&weAreFreshest, &tied);
        ASSERT_FALSE(weAreFreshest);
        ASSERT_FALSE(tied);
        ASSERT_EQUALS(1, countLogLinesContaining("not electing self, h1:27017 would veto with '"
                                                 "errmsg: \"I'd rather you didn't\"'"));
    }

    TEST_F(FreshnessCheckerTest, ElectNotElectingSelfWeAreNotFreshestManyNodes) {
        // one other responds as fresher than us
        startCapturingLogMessages();
        ReplicaSetConfig config = assertMakeRSConfig(
                BSON("_id" << "rs0" <<
                     "version" << 1 <<
                     "members" << BSON_ARRAY(
                         BSON("_id" << 1 << "host" << "h0") <<
                         BSON("_id" << 2 << "host" << "h1") <<
                         BSON("_id" << 3 << "host" << "h2") <<
                         BSON("_id" << 4 << "host" << "h3") <<
                         BSON("_id" << 5 << "host" << "h4"))));
        
        StatusWith<ReplicationExecutor::EventHandle> evh = _executor->makeEvent();
        ASSERT_OK(evh.getStatus());

        Date_t now(0);
        std::vector<HostAndPort> hosts;
        for (ReplicaSetConfig::MemberIterator mem = config.membersBegin();
                mem != config.membersEnd();
                ++mem) {
            hosts.push_back(mem->getHostAndPort());
        }

        const BSONObj freshRequest = makeFreshRequest(config, OpTime(10,0), 0);

        _net->addResponse(RemoteCommandRequest(HostAndPort("h1"),
                                               "admin",
                                               freshRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1 <<
                                                   "id" << 2 <<
                                                   "set" << "rs0" <<
                                                   "who" << "h1" <<
                                                   "cfgver" << 1 <<
                                                   "fresher" << true <<
                                                   "opTime" << Date_t(OpTime(0,0).asDate()))));
        _net->addResponse(RemoteCommandRequest(HostAndPort("h2"),
                                               "admin",
                                               freshRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1 <<
                                                   "id" << 3 <<
                                                   "set" << "rs0" <<
                                                   "who" << "h2" <<
                                                   "cfgver" << 1 <<
                                                   "opTime" << Date_t(OpTime(0,0).asDate()))));
        _net->addResponse(RemoteCommandRequest(HostAndPort("h3"),
                                               "admin",
                                               freshRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1 <<
                                                   "id" << 4 <<
                                                   "set" << "rs0" <<
                                                   "who" << "h3" <<
                                                   "cfgver" << 1 <<
                                                   "opTime" << Date_t(OpTime(0,0).asDate()))));
        _net->addResponse(RemoteCommandRequest(HostAndPort("h4"),
                                               "admin",
                                               freshRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1 <<
                                                   "id" << 5 <<
                                                   "set" << "rs0" <<
                                                   "who" << "h4" <<
                                                   "cfgver" << 1 <<
                                                   "opTime" << Date_t(OpTime(0,0).asDate()))));

        FreshnessChecker checker;
        StatusWith<ReplicationExecutor::CallbackHandle> cbh = 
            _executor->scheduleWork(
                stdx::bind(&FreshnessCheckerTest::freshnessCheckerRunner,
                           this,
                           stdx::placeholders::_1,
                           &checker,
                           evh.getValue(),
                           OpTime(10,0),
                           config,
                           0,
                           hosts));
        ASSERT_OK(cbh.getStatus());
        _executor->wait(cbh.getValue());

        ASSERT_OK(_lastStatus);

        _executor->waitForEvent(evh.getValue());
        stopCapturingLogMessages();

        bool weAreFreshest(true);
        bool tied(true);
        checker.getResults(&weAreFreshest, &tied);
        ASSERT_FALSE(weAreFreshest);
        ASSERT_FALSE(tied);
        ASSERT_EQUALS(1, countLogLinesContaining("not electing self, we are not freshest"));
    }

    /*
    TODO(dannenberg) re-enable this test once we can control message order

    TEST_F(FreshnessCheckerTest, ElectNotElectingSelfWeAreNotFreshestOpTimeManyNodes) {
        // one other responds with a later optime than ours
        startCapturingLogMessages();
        ReplicaSetConfig config = assertMakeRSConfig(
                BSON("_id" << "rs0" <<
                     "version" << 1 <<
                     "members" << BSON_ARRAY(
                         BSON("_id" << 1 << "host" << "h0") <<
                         BSON("_id" << 2 << "host" << "h1") <<
                         BSON("_id" << 3 << "host" << "h2") <<
                         BSON("_id" << 4 << "host" << "h3") <<
                         BSON("_id" << 5 << "host" << "h4"))));
        
        StatusWith<ReplicationExecutor::EventHandle> evh = _executor->makeEvent();
        ASSERT_OK(evh.getStatus());

        Date_t now(0);
        std::vector<HostAndPort> hosts;
        for (ReplicaSetConfig::MemberIterator mem = config.membersBegin();
                mem != config.membersEnd();
                ++mem) {
            hosts.push_back(mem->getHostAndPort());
        }

        const BSONObj freshRequest = makeFreshRequest(config, OpTime(0,0), 0);

        _net->addResponse(RemoteCommandRequest(HostAndPort("h1"),
                                               "admin",
                                               freshRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1 <<
                                                   "id" << 2 <<
                                                   "set" << "rs0" <<
                                                   "who" << "h1" <<
                                                   "cfgver" << 1 <<
                                                   "fresher" << true <<
                                                   "opTime" << Date_t(OpTime(10,0).asDate()))));
        _net->addResponse(RemoteCommandRequest(HostAndPort("h2"),
                                               "admin",
                                               freshRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1 <<
                                                   "id" << 3 <<
                                                   "set" << "rs0" <<
                                                   "who" << "h2" <<
                                                   "cfgver" << 1 <<
                                                   "opTime" << Date_t(OpTime(0,0).asDate()))));
        _net->addResponse(RemoteCommandRequest(HostAndPort("h3"),
                                               "admin",
                                               freshRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1 <<
                                                   "id" << 4 <<
                                                   "set" << "rs0" <<
                                                   "who" << "h3" <<
                                                   "cfgver" << 1 <<
                                                   "opTime" << Date_t(OpTime(0,0).asDate()))));
        _net->addResponse(RemoteCommandRequest(HostAndPort("h4"),
                                               "admin",
                                               freshRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1 <<
                                                   "id" << 5 <<
                                                   "set" << "rs0" <<
                                                   "who" << "h4" <<
                                                   "cfgver" << 1 <<
                                                   "opTime" << Date_t(OpTime(0,0).asDate()))));

        FreshnessChecker checker;
        StatusWith<ReplicationExecutor::CallbackHandle> cbh = 
            _executor->scheduleWork(
                stdx::bind(&FreshnessCheckerTest::freshnessCheckerRunner,
                           this,
                           stdx::placeholders::_1,
                           &checker,
                           evh.getValue(),
                           OpTime(0,0),
                           config,
                           0,
                           hosts));
        ASSERT_OK(cbh.getStatus());
        _executor->wait(cbh.getValue());

        ASSERT_OK(_lastStatus);

        _executor->waitForEvent(evh.getValue());
        stopCapturingLogMessages();

        bool weAreFreshest(true);
        bool tied(true);
        checker.getResults(&weAreFreshest, &tied);
        ASSERT_FALSE(weAreFreshest);
        ASSERT_FALSE(tied);
        ASSERT_EQUALS(1, countLogLinesContaining("not electing self, we are not freshest"));
    }
    */

    TEST_F(FreshnessCheckerTest, ElectWrongTypeInFreshnessResponseManyNodes) {
        // one other responds with "opTime" field of non-Date value, causing not freshest
        startCapturingLogMessages();
        ReplicaSetConfig config = assertMakeRSConfig(
                BSON("_id" << "rs0" <<
                     "version" << 1 <<
                     "members" << BSON_ARRAY(
                         BSON("_id" << 1 << "host" << "h0") <<
                         BSON("_id" << 2 << "host" << "h1") <<
                         BSON("_id" << 3 << "host" << "h2") <<
                         BSON("_id" << 4 << "host" << "h3") <<
                         BSON("_id" << 5 << "host" << "h4"))));
        
        StatusWith<ReplicationExecutor::EventHandle> evh = _executor->makeEvent();
        ASSERT_OK(evh.getStatus());

        Date_t now(0);
        std::vector<HostAndPort> hosts;
        for (ReplicaSetConfig::MemberIterator mem = config.membersBegin();
                mem != config.membersEnd();
                ++mem) {
            hosts.push_back(mem->getHostAndPort());
        }

        const BSONObj freshRequest = makeFreshRequest(config, OpTime(10,0), 0);

        _net->addResponse(RemoteCommandRequest(HostAndPort("h1"),
                                               "admin",
                                               freshRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1 <<
                                                   "id" << 2 <<
                                                   "set" << "rs0" <<
                                                   "who" << "h1" <<
                                                   "cfgver" << 1 <<
                                                   "opTime" << 3)));
        _net->addResponse(RemoteCommandRequest(HostAndPort("h2"),
                                               "admin",
                                               freshRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1 <<
                                                   "id" << 3 <<
                                                   "set" << "rs0" <<
                                                   "who" << "h2" <<
                                                   "cfgver" << 1 <<
                                                   "opTime" << Date_t(OpTime(0,0).asDate()))));
        _net->addResponse(RemoteCommandRequest(HostAndPort("h3"),
                                               "admin",
                                               freshRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1 <<
                                                   "id" << 4 <<
                                                   "set" << "rs0" <<
                                                   "who" << "h3" <<
                                                   "cfgver" << 1 <<
                                                   "opTime" << Date_t(OpTime(0,0).asDate()))));
        _net->addResponse(RemoteCommandRequest(HostAndPort("h4"),
                                               "admin",
                                               freshRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1 <<
                                                   "id" << 5 <<
                                                   "set" << "rs0" <<
                                                   "who" << "h4" <<
                                                   "cfgver" << 1 <<
                                                   "opTime" << Date_t(OpTime(0,0).asDate()))));

        FreshnessChecker checker;
        StatusWith<ReplicationExecutor::CallbackHandle> cbh = 
            _executor->scheduleWork(
                stdx::bind(&FreshnessCheckerTest::freshnessCheckerRunner,
                           this,
                           stdx::placeholders::_1,
                           &checker,
                           evh.getValue(),
                           OpTime(10,0),
                           config,
                           0,
                           hosts));
        ASSERT_OK(cbh.getStatus());
        _executor->wait(cbh.getValue());

        ASSERT_OK(_lastStatus);

        _executor->waitForEvent(evh.getValue());
        stopCapturingLogMessages();

        bool weAreFreshest(true);
        bool tied(true);
        checker.getResults(&weAreFreshest, &tied);
        ASSERT_FALSE(weAreFreshest);
        ASSERT_FALSE(tied);
        ASSERT_EQUALS(1, countLogLinesContaining("wrong type for opTime argument in replSetFresh "
                                                 "response: NumberInt32"));
    }

    TEST_F(FreshnessCheckerTest, ElectVetoedManyNodes) {
        // one other responds with veto
        startCapturingLogMessages();
        ReplicaSetConfig config = assertMakeRSConfig(
                BSON("_id" << "rs0" <<
                     "version" << 1 <<
                     "members" << BSON_ARRAY(
                         BSON("_id" << 1 << "host" << "h0") <<
                         BSON("_id" << 2 << "host" << "h1") <<
                         BSON("_id" << 3 << "host" << "h2") <<
                         BSON("_id" << 4 << "host" << "h3") <<
                         BSON("_id" << 5 << "host" << "h4"))));
        
        StatusWith<ReplicationExecutor::EventHandle> evh = _executor->makeEvent();
        ASSERT_OK(evh.getStatus());

        Date_t now(0);
        std::vector<HostAndPort> hosts;
        for (ReplicaSetConfig::MemberIterator mem = config.membersBegin();
                mem != config.membersEnd();
                ++mem) {
            hosts.push_back(mem->getHostAndPort());
        }

        const BSONObj freshRequest = makeFreshRequest(config, OpTime(10,0), 0);

        _net->addResponse(RemoteCommandRequest(HostAndPort("h1"),
                                               "admin",
                                               freshRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1 <<
                                                   "id" << 2 <<
                                                   "set" << "rs0" <<
                                                   "who" << "h1" <<
                                                   "cfgver" << 1 <<
                                                   "veto" << true <<
                                                   "errmsg" << "I'd rather you didn't" <<
                                                   "opTime" << Date_t(OpTime(0,0).asDate()))));
        _net->addResponse(RemoteCommandRequest(HostAndPort("h2"),
                                               "admin",
                                               freshRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1 <<
                                                   "id" << 3 <<
                                                   "set" << "rs0" <<
                                                   "who" << "h2" <<
                                                   "cfgver" << 1 <<
                                                   "opTime" << Date_t(OpTime(0,0).asDate()))));
        _net->addResponse(RemoteCommandRequest(HostAndPort("h3"),
                                               "admin",
                                               freshRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1 <<
                                                   "id" << 4 <<
                                                   "set" << "rs0" <<
                                                   "who" << "h3" <<
                                                   "cfgver" << 1 <<
                                                   "opTime" << Date_t(OpTime(0,0).asDate()))));
        _net->addResponse(RemoteCommandRequest(HostAndPort("h4"),
                                               "admin",
                                               freshRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1 <<
                                                   "id" << 5 <<
                                                   "set" << "rs0" <<
                                                   "who" << "h4" <<
                                                   "cfgver" << 1 <<
                                                   "opTime" << Date_t(OpTime(0,0).asDate()))));

        FreshnessChecker checker;
        StatusWith<ReplicationExecutor::CallbackHandle> cbh = 
            _executor->scheduleWork(
                stdx::bind(&FreshnessCheckerTest::freshnessCheckerRunner,
                           this,
                           stdx::placeholders::_1,
                           &checker,
                           evh.getValue(),
                           OpTime(10,0),
                           config,
                           0,
                           hosts));
        ASSERT_OK(cbh.getStatus());
        _executor->wait(cbh.getValue());

        ASSERT_OK(_lastStatus);

        _executor->waitForEvent(evh.getValue());
        stopCapturingLogMessages();

        bool weAreFreshest(true);
        bool tied(true);
        checker.getResults(&weAreFreshest, &tied);
        ASSERT_FALSE(weAreFreshest);
        ASSERT_FALSE(tied);
        ASSERT_EQUALS(1, countLogLinesContaining("not electing self, h1:27017 would veto with '"
                                                 "errmsg: \"I'd rather you didn't\"'"));
    }

    /*
    TODO(dannenberg) re-enable this test once we can control message order

    TEST_F(FreshnessCheckerTest, ElectVetoedAndTiedFreshnessManyNodes) {
        // one other responds with veto and another responds with tie
        startCapturingLogMessages();
        ReplicaSetConfig config = assertMakeRSConfig(
                BSON("_id" << "rs0" <<
                     "version" << 1 <<
                     "members" << BSON_ARRAY(
                         BSON("_id" << 1 << "host" << "h0") <<
                         BSON("_id" << 2 << "host" << "h1") <<
                         BSON("_id" << 3 << "host" << "h2") <<
                         BSON("_id" << 4 << "host" << "h3") <<
                         BSON("_id" << 5 << "host" << "h4"))));
        
        StatusWith<ReplicationExecutor::EventHandle> evh = _executor->makeEvent();
        ASSERT_OK(evh.getStatus());

        Date_t now(0);
        std::vector<HostAndPort> hosts;
        for (ReplicaSetConfig::MemberIterator mem = config.membersBegin();
                mem != config.membersEnd();
                ++mem) {
            hosts.push_back(mem->getHostAndPort());
        }

        const BSONObj freshRequest = makeFreshRequest(config, OpTime(10,0), 0);

        _net->addResponse(RemoteCommandRequest(HostAndPort("h1"),
                                               "admin",
                                               freshRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1 <<
                                                   "id" << 2 <<
                                                   "set" << "rs0" <<
                                                   "who" << "h1" <<
                                                   "cfgver" << 1 <<
                                                   "veto" << true <<
                                                   "errmsg" << "I'd rather you didn't" <<
                                                   "opTime" << Date_t(OpTime(0,0).asDate()))));
        _net->addResponse(RemoteCommandRequest(HostAndPort("h2"),
                                               "admin",
                                               freshRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1 <<
                                                   "id" << 3 <<
                                                   "set" << "rs0" <<
                                                   "who" << "h2" <<
                                                   "cfgver" << 1 <<
                                                   "opTime" << Date_t(OpTime(10,0).asDate()))));
        _net->addResponse(RemoteCommandRequest(HostAndPort("h3"),
                                               "admin",
                                               freshRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1 <<
                                                   "id" << 4 <<
                                                   "set" << "rs0" <<
                                                   "who" << "h3" <<
                                                   "cfgver" << 1 <<
                                                   "opTime" << Date_t(OpTime(0,0).asDate()))));
        _net->addResponse(RemoteCommandRequest(HostAndPort("h4"),
                                               "admin",
                                               freshRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1 <<
                                                   "id" << 5 <<
                                                   "set" << "rs0" <<
                                                   "who" << "h4" <<
                                                   "cfgver" << 1 <<
                                                   "opTime" << Date_t(OpTime(0,0).asDate()))));

        FreshnessChecker checker;
        StatusWith<ReplicationExecutor::CallbackHandle> cbh = 
            _executor->scheduleWork(
                stdx::bind(&FreshnessCheckerTest::freshnessCheckerRunner,
                           this,
                           stdx::placeholders::_1,
                           &checker,
                           evh.getValue(),
                           OpTime(10,0),
                           config,
                           0,
                           hosts));
        ASSERT_OK(cbh.getStatus());
        _executor->wait(cbh.getValue());

        ASSERT_OK(_lastStatus);

        _executor->waitForEvent(evh.getValue());
        stopCapturingLogMessages();

        bool weAreFreshest(true);
        bool tied(true);
        checker.getResults(&weAreFreshest, &tied);
        ASSERT_FALSE(weAreFreshest);
        ASSERT_FALSE(tied);
        ASSERT_EQUALS(1, countLogLinesContaining("not electing self, h1:27017 would veto with '"
                                                 "errmsg: \"I'd rather you didn't\"'"));
    }
    */

    TEST_F(FreshnessCheckerTest, ElectManyNodesNotAllRespond) {
        startCapturingLogMessages();
        ReplicaSetConfig config = assertMakeRSConfig(
                BSON("_id" << "rs0" <<
                     "version" << 1 <<
                     "members" << BSON_ARRAY(
                         BSON("_id" << 1 << "host" << "h0") <<
                         BSON("_id" << 2 << "host" << "h1") <<
                         BSON("_id" << 3 << "host" << "h2") <<
                         BSON("_id" << 4 << "host" << "h3") <<
                         BSON("_id" << 5 << "host" << "h4"))));
        
        StatusWith<ReplicationExecutor::EventHandle> evh = _executor->makeEvent();
        ASSERT_OK(evh.getStatus());

        Date_t now(0);
        std::vector<HostAndPort> hosts;
        for (ReplicaSetConfig::MemberIterator mem = config.membersBegin();
                mem != config.membersEnd();
                ++mem) {
            hosts.push_back(mem->getHostAndPort());
        }

        const BSONObj freshRequest = makeFreshRequest(config, OpTime(10,0), 0);

        _net->addResponse(RemoteCommandRequest(HostAndPort("h1"),
                                               "admin",
                                               freshRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1 <<
                                                   "id" << 2 <<
                                                   "set" << "rs0" <<
                                                   "who" << "h1" <<
                                                   "cfgver" << 1 <<
                                                   "opTime" << Date_t(OpTime(0,0).asDate()))));
        _net->addResponse(RemoteCommandRequest(HostAndPort("h4"),
                                               "admin",
                                               freshRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1 <<
                                                   "id" << 5 <<
                                                   "set" << "rs0" <<
                                                   "who" << "h4" <<
                                                   "cfgver" << 1 <<
                                                   "opTime" << Date_t(OpTime(0,0).asDate()))));

        FreshnessChecker checker;
        StatusWith<ReplicationExecutor::CallbackHandle> cbh = 
            _executor->scheduleWork(
                stdx::bind(&FreshnessCheckerTest::freshnessCheckerRunner,
                           this,
                           stdx::placeholders::_1,
                           &checker,
                           evh.getValue(),
                           OpTime(10,0),
                           config,
                           0,
                           hosts));
        ASSERT_OK(cbh.getStatus());
        _executor->wait(cbh.getValue());

        ASSERT_OK(_lastStatus);

        _executor->waitForEvent(evh.getValue());
        stopCapturingLogMessages();

        bool weAreFreshest(false);
        bool tied(true);
        checker.getResults(&weAreFreshest, &tied);
        ASSERT_TRUE(weAreFreshest);
        ASSERT_FALSE(tied);
    }

}  // namespace
}  // namespace repl
}  // namespace mongo
