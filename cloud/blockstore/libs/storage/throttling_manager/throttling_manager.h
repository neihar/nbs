#pragma once

#include "public.h"

#include <cloud/blockstore/libs/storage/core/public.h>

namespace NCloud::NBlockStore::NStorage {

////////////////////////////////////////////////////////////////////////////////

NActors::IActorPtr CreateThrottlingManager();

}   // namespace NCloud::NBlockStore::NStorage
