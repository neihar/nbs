#include "fs_impl.h"
#include "fs.h"

#include <cloud/storage/core/libs/common/scheduler_test.h>
#include <library/cpp/testing/unittest/registar.h>
#include <cloud/storage/core/libs/diagnostics/logging.h>
#include <cloud/storage/core/libs/common/timer.h>
#include <util/folder/tempdir.h>
#include <util/folder/path.h>
#include <cloud/filestore/libs/diagnostics/profile_log.h>
#include <cloud/filestore/libs/diagnostics/public.h>
#include <cloud/filestore/libs/service/filestore_test.h>
#include <cloud/filestore/libs/client/session.h>
#include <cloud/filestore/libs/client/config.h>

namespace NCloud::NFileStore::NFuse {

namespace {

////////////////////////////////////////////////////////////////////////////////

class TBootstrap
{
public:
    TBootstrap()
        : Logging(CreateLoggingService(
              "console",
              {.FiltrationLevel = TLOG_RESOURCES}))
        , ProfileLog(CreateProfileLogStub())
        , Scheduler(new TTestScheduler())
        , Timer(CreateWallClockTimer())
        , Stats(nullptr)
        , Queue(nullptr)

    {
        NProto::TFileSystemConfig protoConfig;
        protoConfig.SetFileSystemId("tst");
        Config = std::make_shared<TFileSystemConfig>(protoConfig);

        Service = std::make_shared<TFileStoreTest>();

        auto sessionConfig =
            std::make_shared<NCloud::NFileStore::NClient::TSessionConfig>(
                NProto::TSessionConfig{});

        Session = NCloud::NFileStore::NClient::CreateSession(
            Logging,
            Timer,
            Scheduler,
            Service,
            sessionConfig);

        THandleOpsQueuePtr handleOpsQueue(CreateHandleOpsQueue("tst", 1));
        TWriteBackCachePtr writeBackCache(CreateWriteBackCache("tst", 1));

        FileSystem = CreateFileSystem(
            Logging,
            ProfileLog,
            Scheduler,
            Timer,
            Config,
            Session,
            Stats,
            Queue,
            std::move(handleOpsQueue),
            std::move(writeBackCache));
    }

    ~TBootstrap()
    {
    }

    IFileSystemPtr GetFs()
    {
        return FileSystem;
    }

private:

    ILoggingServicePtr Logging;
    NCloud::NFileStore::IProfileLogPtr ProfileLog;
    ISchedulerPtr Scheduler;
    ITimerPtr Timer;
    TFileSystemConfigPtr Config;
    IFileStorePtr Session;
    IRequestStatsPtr Stats;
    ICompletionQueuePtr Queue;

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
