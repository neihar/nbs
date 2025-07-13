#include "throttling_manager.h"

#include <cloud/blockstore/config/throttling.pb.h>
#include <cloud/blockstore/libs/kikimr/events.h>
#include <cloud/blockstore/libs/storage/api/service.h>
#include <cloud/blockstore/libs/storage/api/throttling_manager.h>

#include <cloud/storage/core/libs/actors/helpers.h>
#include <cloud/storage/core/libs/common/proto_helpers.h>

#include <contrib/ydb/library/actors/core/actor.h>
#include <contrib/ydb/library/actors/core/actor_bootstrapped.h>
#include <contrib/ydb/library/actors/core/events.h>
#include <contrib/ydb/library/actors/core/hfunc.h>
#include <contrib/ydb/library/actors/core/log.h>

namespace NCloud::NBlockStore::NStorage {

using TThrottlerConfigPtr = std::unique_ptr<NProto::TThrottlingConfig>;
using TThrottlingItems =
    google::protobuf::RepeatedPtrField<NProto::TThrottlingRule>;

using namespace NActors;

namespace {

////////////////////////////////////////////////////////////////////////////////

struct TThrottlerManagerConfig
{
    TDuration CycleTime;
    TDuration ServiceActorResponseTimeout;
    TString ConfigPath;
};

using TThrottlerManagerConfigPtr = std::unique_ptr<TThrottlerManagerConfig>;

////////////////////////////////////////////////////////////////////////////////

struct TEvThrottlingManager
{
    struct TUpdateFromFile
    {
    };

    struct TNotifyVolume
    {
        TThrottlingItems Config;
    };

    enum EEvents
    {
        EvBegin = EventSpaceBegin(TEvents::ES_USERSPACE),

        EvUpdateFromFile,

        EvNotifyVolume,

        EvEnd
    };

    using TEvUpdateFromFile = TRequestEvent<TUpdateFromFile, EvUpdateFromFile>;
    using TEvNotifyVolume = TRequestEvent<TNotifyVolume, EvNotifyVolume>;
};

////////////////////////////////////////////////////////////////////////////////

class TThrottlingManagerActor final
    : public NActors::TActorBootstrapped<TThrottlingManagerActor>
{
private:
    const TThrottlerManagerConfigPtr Config;
    TThrottlerConfigPtr ThrottlingConfig;

public:
    void Bootstrap(const TActorContext& ctx)
    {
        Become(&TThis::StateWork);

        if (!Config->ConfigPath) {
            LOG_ERROR(
                ctx,
                TBlockStoreComponents::SERVICE,
                "Throttler config path is empty");
            // TODO: Do I need to handle dying?
            Die(ctx);
            return;
        }

        NCloud::Send(
            ctx,
            ctx.SelfID,
            std::make_unique<TEvThrottlingManager::TEvUpdateFromFile>());
    }

private:
    TResultOrError<TThrottlerConfigPtr> ReadThrottlerConfigFromFile()
    try {
        NProto::TThrottlingConfig config;
        // TODO: Maybe use SafeExecute. Also check if it locks file
        ParseProtoTextFromFile(Config->ConfigPath, config);
        return std::make_unique<NProto::TThrottlingConfig>(std::move(config));
    } catch (std::exception& e) {
        return MakeError(E_FAIL, CurrentExceptionMessage());
    }

    void UpdateConfig(
        const NActors::TActorContext& ctx,
        TThrottlerConfigPtr newConfig)
    {
        if (!newConfig) {
            LOG_WARN(
                ctx,
                TBlockStoreComponents::SERVICE,
                "New throttler config is empty");
            return;
        }

        if (!ThrottlingConfig) {
            ThrottlingConfig = std::move(newConfig);
            return;
        }

        if (ThrottlingConfig->GetVersion() >= newConfig->GetVersion()) {
            LOG_DEBUG(
                ctx,
                TBlockStoreComponents::SERVICE,
                "New throttler config version (%lu) is less than the known "
                "one's (%lu)",
                newConfig->GetVersion(),
                ThrottlingConfig->GetVersion());
            return;
        }

        ThrottlingConfig = std::move(newConfig);
    }

private:
    STFUNC(StateWork)
    {
        switch (ev->GetTypeRewrite()) {
            HFunc(
                TEvThrottlingManager::TEvUpdateFromFile,
                HandleUpdateConfigFromFile);
            HFunc(
                TEvService::TEvListVolumesResponse,
                HandleListVolumesResponse);
            default:
                HandleUnexpectedEvent(
                    ev,
                    TBlockStoreComponents::SERVICE,
                    __PRETTY_FUNCTION__);
                break;
        }
    }

    void HandleUpdateConfigFromFile(
        TEvThrottlingManager::TEvUpdateFromFile::TPtr& /*unused*/,
        const NActors::TActorContext& ctx)
    {
        auto maybeConfig = ReadThrottlerConfigFromFile();

        if (maybeConfig.HasError()) {
            LOG_WARN(
                ctx,
                TBlockStoreComponents::SERVICE,
                "Unable to read throttler config: %s",
                CurrentExceptionMessage().Quote().c_str());
        } else {
            UpdateConfig(ctx, maybeConfig.ExtractResult());
        }

        if (ThrottlingConfig) {
            // Send notification each cycle. It simplifies logic, and there's
            // not much mounted volumes on one host anyway
            // TODO: list mounted volumes
            NCloud::Send(
                ctx,
                MakeStorageServiceId(),
                std::make_unique<TEvService::TEvListVolumesRequest>());
        }

        auto request =
            std::make_unique<TEvThrottlingManager::TEvUpdateFromFile>();

        ctx.Schedule(Config->CycleTime, request.release());
    }

    // TODO: HandleListVolumesResponse -> HandleListMountedVolumesResponse
    void HandleListVolumesResponse(
        TEvService::TEvListVolumesResponse::TPtr& ev,
        const NActors::TActorContext& ctx)
    {
        auto* event = ev->Get();
        if (event->Record.HasError()) {
            LOG_WARN(
                ctx,
                TBlockStoreComponents::SERVICE,
                "Failed to list mounted volumes: %s",
                CurrentExceptionMessage().Quote().c_str());
        }

        auto request =
            std::make_shared<TEvThrottlingManager::TEvNotifyVolume>();
        request->Config = ThrottlingConfig->GetRules();

        for (const auto& actor: event->Record.GetVolumes()) {
            ctx.Send(actor, request.get());
        }
    }
};

}   // namespace

////////////////////////////////////////////////////////////////////////////////

NActors::IActorPtr CreateThrottlingManager()
{
    // TODO: form TThrottlerManagerConfig from storage config
    return std::make_unique<TThrottlingManagerActor>();
}

}   // namespace NCloud::NBlockStore::NStorage
