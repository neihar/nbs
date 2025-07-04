#include "fs_impl.h"

#include "fs.h"

#include <cloud/filestore/libs/client/config.h>
#include <cloud/filestore/libs/client/session.h>
#include <cloud/filestore/libs/diagnostics/profile_log.h>
#include <cloud/filestore/libs/diagnostics/public.h>
#include <cloud/filestore/libs/service/filestore_test.h>

#include <cloud/storage/core/libs/common/scheduler_test.h>
#include <cloud/storage/core/libs/common/timer.h>
#include <cloud/storage/core/libs/diagnostics/logging.h>

#include <library/cpp/testing/unittest/registar.h>

#include <util/folder/path.h>
#include <util/folder/tempdir.h>
#include <library/cpp/monlib/dynamic_counters/counters.h>
#include <cloud/storage/core/libs/user_stats/counter/user_counter.h>

namespace NCloud::NFileStore::NFuse {

namespace {

using namespace NCloud::NStorage::NUserStats;
using namespace NMonitoring;
using namespace NCloud;

////////////////////////////////////////////////////////////////////////////////

class TBootstrap
{
public:
    TBootstrap()
    {
        auto logFs= CreateLoggingService("fs");
        NCloud::NFileStore::IProfileLogPtr profileLogFs(CreateProfileLogStub());
        ISchedulerPtr schedulerFs(new TTestScheduler());
        ITimerPtr timerFs(CreateWallClockTimer());

        NProto::TFileSystemConfig protoConfig;
        protoConfig.SetFileSystemId("tst_fs_id");
        TFileSystemConfigPtr configFs = std::make_shared<TFileSystemConfig>(protoConfig);

        auto logSession= CreateLoggingService("session");
        ITimerPtr timerSession(CreateWallClockTimer());
        ISchedulerPtr schedulerSession(new TTestScheduler());
        auto serviceSession = std::make_shared<TFileStoreTest>();
        auto sessionConfig =
            std::make_shared<NCloud::NFileStore::NClient::TSessionConfig>(
                NProto::TSessionConfig{});
        IFileStorePtr sessionFs = NCloud::NFileStore::NClient::CreateSession(
            logSession,
            timerSession,
            schedulerSession,
            serviceSession,
            sessionConfig);

    TDynamicCountersPtr countersStats = MakeIntrusive<TDynamicCounters>();
    ITimerPtr timerStats = CreateWallClockTimer();
    IRequestStatsRegistryPtr registryStats = CreateRequestStatsRegistryStub();

        IRequestStatsPtr statsFs = registryStats->GetFileSystemStats(
            configFs->GetFileSystemId(),
            "tst_client");

        //ICompletionQueuePtr Queue;

//        THandleOpsQueuePtr handleOpsQueue(CreateHandleOpsQueue("tst2", 1));
//        TWriteBackCachePtr writeBackCache(CreateWriteBackCache("tst3", 1));

        //        FileSystem = CreateFileSystem(
        //            logFs,
        //            profileLogFs,
        //            schedulerFs,
        //            timerFs,
        //            configFs,
        //            sessionFs,
        //            Stats,
        //            Queue,
        //            std::move(handleOpsQueue),
        //            std::move(writeBackCache));
    }

    ~TBootstrap()
    {}

    IFileSystemPtr GetFs()
    {
        return FileSystem;
    }

private:
    std::shared_ptr<TFileStoreTest> Service;

    IFileSystemPtr FileSystem;
};

}   // namespace

////////////////////////////////////////////////////////////////////////////////

Y_UNIT_TEST_SUITE(TFileSystemImplTest)
{
    Y_UNIT_TEST(ShouldCritOnCreate)
    {
        TBootstrap bootstrap;
        auto fs = bootstrap.GetFs();
    }
}

}   // namespace NCloud::NFileStore::NFuse
