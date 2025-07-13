#pragma once

#include "public.h"

#include <contrib/ydb/library/actors/core/actorid.h>

namespace NCloud::NBlockStore::NStorage {

NActors::TActorId MakeThrottlingManagerServiceId();

}   // namespace NCloud::NBlockStore::NStorage
