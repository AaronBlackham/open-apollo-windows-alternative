/*
 * Build-time check only (no code): apollo_ioctl_min.h against the apollo_ioctl.h of the driver being
 * packaged. build.cmd puts that header's folder on the include path (the payload's src\include).
 */
#include <stddef.h>
#include "apollo_ioctl_min.h"
#include <apollo_ioctl.h>

static_assert(HELPER_IOCTL_GET_ENGINE_STATS == IOCTL_APOLLO_GET_ENGINE_STATS, "GET_ENGINE_STATS code changed");
static_assert(HELPER_IOCTL_GET_ENDPOINTS == IOCTL_APOLLO_GET_ENDPOINTS, "GET_ENDPOINTS code changed");
static_assert(HELPER_ASIO_STATE_OPEN == APOLLO_ASIO_STATE_OPEN, "ASIO state values changed");
static_assert(HELPER_ASIO_STATE_RUNNING == APOLLO_ASIO_STATE_RUNNING, "ASIO state values changed");
static_assert(HELPER_EP_FLAG_PREPARED == APOLLO_EP_FLAG_PREPARED, "endpoint flags changed");
static_assert(HELPER_EP_FLAG_RUNNING == APOLLO_EP_FLAG_RUNNING, "endpoint flags changed");
static_assert(HELPER_ENDPOINTS_MAX == APOLLO_ENDPOINTS_MAX, "APOLLO_ENDPOINTS_MAX changed");

static_assert(sizeof(HELPER_ENGINE_STATS) == sizeof(APOLLO_ENGINE_STATS), "APOLLO_ENGINE_STATS size changed");
static_assert(offsetof(HELPER_ENGINE_STATS, AsioState) == offsetof(APOLLO_ENGINE_STATS, AsioState),
              "APOLLO_ENGINE_STATS.AsioState moved");
static_assert(sizeof(HELPER_ENDPOINT_INFO) == sizeof(APOLLO_ENDPOINT_INFO), "APOLLO_ENDPOINT_INFO size changed");
static_assert(offsetof(HELPER_ENDPOINT_INFO, Flags) == offsetof(APOLLO_ENDPOINT_INFO, Flags),
              "APOLLO_ENDPOINT_INFO.Flags moved");
static_assert(offsetof(HELPER_ENDPOINT_INFO, Name) == offsetof(APOLLO_ENDPOINT_INFO, Name),
              "APOLLO_ENDPOINT_INFO.Name moved");
static_assert(sizeof(HELPER_ENDPOINTS) == sizeof(APOLLO_ENDPOINTS), "APOLLO_ENDPOINTS size changed");
static_assert(offsetof(HELPER_ENDPOINTS, Prepared) == offsetof(APOLLO_ENDPOINTS, Prepared),
              "APOLLO_ENDPOINTS.Prepared moved");
static_assert(offsetof(HELPER_ENDPOINTS, Endpoint) == offsetof(APOLLO_ENDPOINTS, Endpoint),
              "APOLLO_ENDPOINTS.Endpoint moved");

/* GET_INFO: the version 5 part is in every packaged driver; the unit fields from version 6. */
static_assert(HELPER_IOCTL_GET_INFO == IOCTL_APOLLO_GET_INFO, "GET_INFO code changed");
static_assert(offsetof(HELPER_INFO, Version) == offsetof(APOLLO_INFO, Version), "APOLLO_INFO.Version moved");
static_assert(offsetof(HELPER_INFO, SubsystemId) == offsetof(APOLLO_INFO, SubsystemId),
              "APOLLO_INFO.SubsystemId moved");
static_assert(offsetof(HELPER_INFO, DeviceType) == offsetof(APOLLO_INFO, DeviceType),
              "APOLLO_INFO.DeviceType moved");
static_assert(offsetof(HELPER_INFO, Serial) == offsetof(APOLLO_INFO, Serial), "APOLLO_INFO.Serial moved");
#if APOLLO_IOCTL_VERSION >= 6
static_assert(sizeof(HELPER_INFO) == sizeof(APOLLO_INFO), "APOLLO_INFO size changed");
static_assert(HELPER_INFO_V5_BYTES == APOLLO_INFO_V5_BYTES, "APOLLO_INFO_V5_BYTES changed");
static_assert(offsetof(HELPER_INFO, Model) == offsetof(APOLLO_INFO, Model), "APOLLO_INFO.Model moved");
static_assert(offsetof(HELPER_INFO, Mode) == offsetof(APOLLO_INFO, Mode), "APOLLO_INFO.Mode moved");
static_assert(offsetof(HELPER_INFO, UnitFlags) == offsetof(APOLLO_INFO, UnitFlags), "APOLLO_INFO.UnitFlags moved");
static_assert(offsetof(HELPER_INFO, ModelName) == offsetof(APOLLO_INFO, ModelName), "APOLLO_INFO.ModelName moved");
static_assert(offsetof(HELPER_INFO, UnitKey) == offsetof(APOLLO_INFO, UnitKey), "APOLLO_INFO.UnitKey moved");
static_assert(sizeof(((HELPER_INFO *)0)->ModelName) == sizeof(((APOLLO_INFO *)0)->ModelName), "ModelName size");
static_assert(sizeof(((HELPER_INFO *)0)->UnitKey) == sizeof(((APOLLO_INFO *)0)->UnitKey), "UnitKey size");
static_assert(HELPER_TYPE_X8P_ID == APOLLO_TYPE_X8P_ID && HELPER_TYPE_TWINX_ID == APOLLO_TYPE_TWINX_ID,
              "device type values changed");
static_assert(HELPER_INFO_MODE_UNKNOWN == APOLLO_INFO_MODE_UNKNOWN, "APOLLO_INFO_MODE_UNKNOWN changed");
static_assert(HELPER_UNIT_FLAG_RESOLVED == APOLLO_UNIT_FLAG_RESOLVED && HELPER_UNIT_FLAG_ACTIVE == APOLLO_UNIT_FLAG_ACTIVE &&
              HELPER_UNIT_FLAG_OBSERVE == APOLLO_UNIT_FLAG_OBSERVE && HELPER_UNIT_FLAG_REFUSED == APOLLO_UNIT_FLAG_REFUSED,
              "unit flags changed");
#else
/* Version 5: the struct ends at Reserved[3]; the helper reads no unit fields from it. */
static_assert(sizeof(APOLLO_INFO) == HELPER_INFO_V5_BYTES, "version 5 APOLLO_INFO size changed");
#endif
