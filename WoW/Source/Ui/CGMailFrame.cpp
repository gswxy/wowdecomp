#include "CGMailFrame.h"
#include <cstring>
#include "Engine/Source/FrameScript/FrameScript.h"
#include "WoW/Source/Object/ObjectClient/Player_C.h"
#include "WoW/Source/Ui/GameUI.h"
#include "WoW/Source/Ui/MailInfo.h"
#include "WoW/Source/Ui/Types.h"

// VERIFY: 587c3b2c manual
// OFFSET: 0x0056D4D0
void CGMailInfo::ClearSendMail(bool signal) {
    for (uint32_t i = 0; i < CGMailInfo::NUM_SEND_ITEM_SLOTS; i++) {
        if (CGMailInfo::m_sendItems[i].m_guid) {
            CGGameUI::UnlockItem(CGMailInfo::m_sendItems[i].m_guid);
        }
    }

    memset(CGMailInfo::m_sendItems, 0, sizeof(CGMailInfo::m_sendItems));
    CGMailInfo::m_sendMoney = 0;
    CGMailInfo::m_lockSendItems = 0;
    CGMailInfo::m_sendCOD = 0;
    CGMailInfo::m_sendPending = 0;
    CGMailInfo::m_selectedStationery = 0;
    CGMailInfo::m_selectedPackage = 0;

    if (signal) {
        FrameScript_SignalEvent(SCRIPT_EVENT_SEND_MAIL_MONEY_CHANGED, nullptr);
        FrameScript_SignalEvent(SCRIPT_EVENT_SEND_MAIL_COD_CHANGED, nullptr);
        FrameScript_SignalEvent(SCRIPT_EVENT_MAIL_SEND_SUCCESS, nullptr);
    }
}

// VERIFY: 41ce70d8 auto
// OFFSET: 0x0056DA60
void CGPlayer_C::TalkToMailbox(WOWGUID guid) {
    CGGameUI::SetInteractTarget(guid);
    CGMailInfo::m_object = guid;
    CGMailInfo::ClearSendMail(true);
    FrameScript_SignalEvent(SCRIPT_EVENT_MAIL_SHOW, nullptr);
}
