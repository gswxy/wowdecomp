#ifndef WOW_SOURCE_OBJECT_OBJECTCLIENT_MOVEMENT_C_H
#define WOW_SOURCE_OBJECT_OBJECTCLIENT_MOVEMENT_C_H

#include <cstddef>
#include <cstdint>
#include "WoW/Source/Object/ObjectClient/Object_C.h"
class CMoveSpline;

class C3Vector;
class C44Matrix;
class C4Quaternion;
class CGGameObject_C;
class CMovementGlobals;
class CPassenger;

void MovementAddTransport(CGGameObject_C* transport);
// New (not in the original): walk the transports MovementAddTransport linked, nullptr = the first.
CGGameObject_C* MovementNextTransport(CGGameObject_C* after);
int32_t MovementAddToTransport(CPassenger* passenger, WOWGUID transport, int32_t reason);
CMovementGlobals* MovementGetGlobals();
uint32_t MovementGetIdleTime();
void MovementSetGlobals(CMovementGlobals* globals);
int32_t MovementGetLastUpdateTime(uint32_t* time);
float MovementGetTransportFacing(WOWGUID transport);
int32_t MovementGetTransportMtxSafe(WOWGUID transport, C44Matrix* matrix);
void MovementSetTransportUpdateTime(uint32_t time);
int32_t MovementGetTransportMtxX(WOWGUID transport, C44Matrix* matrix, WOWGUID owner, const char* file, int32_t line);
void MovementUpdateCameraYaw(WOWGUID guid, WOWGUID transport);
void MovementGetTransportQuat(WOWGUID transport, C4Quaternion* quat);
int32_t MovementInsideTransport(WOWGUID transport, const C3Vector& point);
void MovementMoveTransports(uint32_t time, uint32_t elapsed);
void MovementNotifyTransportUnitRemoved(WOWGUID passenger, WOWGUID transport, uint8_t seat);
void MovementNotifyZoneMgr(WOWGUID guid);
int32_t MovementObjIsTransport(WOWGUID guid);
void MovementRemoveTransport(CGGameObject_C* transport);

void MovementDestroy();
int32_t MovementIdleMoveUnits(const void* data, void* param);
void MovementDelSpline(CMoveSpline* spline);
CMoveSpline* MovementNewSpline();
void MovementUpdateWorld(WOWGUID guid, const C3Vector& position, int32_t preload);
void MovementInitialize(const char* logFile);

#endif
