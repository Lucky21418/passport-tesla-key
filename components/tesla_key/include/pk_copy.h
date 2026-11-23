#ifndef PK_COPY_H
#define PK_COPY_H

#include "pk_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * M15: user-visible string table. Wording follows the contract; do not
 * change gates, key mapping, or driving-writable navigation while polishing.
 * This module does not edit official Stitch pages. t816u discarded
 * main-screen vehicle identity as a requirement; it is not a copy item.
 */

typedef enum {
    PK_MSG_EMPTY = 0,
    PK_MSG_PRESSED,                    /* 已按下●，等待手势 */
    PK_MSG_CANCELLED,                  /* 已取消，未发送 */
    PK_MSG_BUSY_NO_QUEUE,              /* 处理中，不排队 */
    PK_MSG_RECONNECT_FIRST,            /* 请先重连 */
    PK_MSG_NO_TARGET,                  /* 未添加车辆 */
    PK_MSG_NOT_PAIRED,                 /* 尚未完成配对 */
    PK_MSG_UNQUALIFIED,                /* 挡位／车速未确认或已过期 */
    PK_MSG_MOVING,                     /* 行驶中不可用 */
    PK_MSG_CAPABILITY,                 /* 能力待确认／不支持 */
    PK_MSG_LOCK_SNAP_BLOCK,            /* 锁快照未确认，不能反向开锁 */
    PK_MSG_CABLE,                      /* 插枪状态未确认或电缆已连接 */
    PK_MSG_CAR_WASH,                   /* 洗车模式或状态未确认 */
    PK_MSG_NEED_CLIMATE,               /* 请先单独开启空调，再新操作 */
    PK_MSG_OLD_GESTURE,                /* 旧手势已清除，请松开后重新操作 */
    PK_MSG_SENT_WAIT,                  /* command written; awaiting vehicle */
    PK_MSG_HOLD_DANGER,                /* dangerous row: long press to execute */
    PK_MSG_STATUS_READONLY,            /* 状态只读，不发送车控 */
    PK_MSG_DRV_MANAGEMENT,             /* 行驶中不可进入车辆管理 */
    PK_MSG_GRANT_EXPIRED,              /* 驻车资格已过期，请重新检查 */
    PK_MSG_QUAL_IN_PROGRESS,           /* 正在读取驻车资格 */
    PK_MSG_QUAL_OK,                    /* G-02: 已检查P与静止，请松开后重新操作 */
    PK_MSG_QUAL_MOVING,                /* 行驶中不可用；请用原厂卡 */
    PK_MSG_WAKE_OFFER,                 /* 未读全／超时；●唤醒车辆并检查，▼返回。不自动唤醒 */
    PK_MSG_WAKE_OFFER_UNKNOWN,         /* 挡位／车速未确认；可用原厂卡 */
    PK_MSG_WAKE_AGAIN,                 /* 读取不足，车可能休眠 */
    PK_MSG_WAKE_DECLINED,              /* 已返回，未唤醒，不补发旧操作 */
    PK_MSG_WAKE_FAIL,                  /* 唤醒未确认，请用原厂卡 */
    PK_MSG_WAKE_TIMEOUT,               /* 唤醒或读取未确认，请用原厂卡；不自动再唤醒 */
    PK_MSG_QUAL_NEED,                  /* 无车辆／请先重连／处理中 */
    PK_MSG_WAIT_EXIT,                  /* 已退出本地等待，不撤销已发动作 */
    PK_MSG_CMD_WAIT_MID,               /* 车辆处理中，继续本轮有限等待 */
    PK_MSG_CMD_UNRECOGNIZED,           /* 响应未识别，仍受原截止限制 */
    PK_MSG_CMD_ACCEPTED,               /* 请求已接受，请查看状态 */
    PK_MSG_CMD_REJECTED,               /* 车辆已拒绝，未自动重发 */
    PK_MSG_CMD_UNCONFIRMED,            /* 结果未确认，请先现场查看 */
    PK_MSG_CMD_TIMEOUT,                /* 未确认，请现场查看；不自动重发 */
    PK_MSG_AUTH_REJECTED,              /* 授权请求被拒绝，请检查车内设置／PIN */
    PK_MSG_AUTH_UNCONFIRMED,           /* 授权请求未确认，请车内确认 */
    PK_MSG_AUTH_EXPIRED,               /* 授权请求提示已过期，请车内确认 */
    PK_MSG_LOCK_VERIFYING,             /* 正在确认锁定 */
    PK_MSG_LOCK_CONFIRMED,             /* G-05: 已确认锁定 */
    PK_MSG_LOCK_UNLOCKED,              /* 当前未锁定 */
    PK_MSG_LOCK_UNCONFIRMED,           /* 锁车未确认 */
    PK_MSG_LOCK_NO_LINK,               /* 锁车未确认，车可能已锁，请现场确认 */
    PK_MSG_CLOSURES_MANY,              /* 多处门盖未关 */
    PK_MSG_CLOSURES_SOME_UNKNOWN,      /* 另有状态未确认 */
    PK_MSG_MGMT_BUSY_OR_MOVING,        /* 行驶中或处理中，不可管理车辆 */
    PK_MSG_MGMT_CONNECTED_UNKNOWN,     /* 已连接且运动／资格未知，不能拆除可能仍需的驾驶连接 */
    PK_MSG_SWITCHED,                   /* 切车后需重新连接和检查 */
    PK_MSG_RECORD_UPDATED,             /* 本机记录已更新；车端登记未撤销 */
    PK_MSG_DELETE_INCOMPLETE,          /* 删除未完成，保留旧配置 */
    PK_MSG_DELETE_RECOVERED,           /* 已恢复提交后的本机配置；车端登记未撤销 */
    PK_MSG_FULL_OR_PROTECTED,          /* 车辆已满或当前驾驶会话受保护 */
    PK_MSG_VIN_OK,                     /* 车架号已录入，尚未验证实车 */
    PK_MSG_VIN_BAD,                    /* 格式／重复／容量错误，请重新录入 */
    PK_MSG_TARGET_UNVERIFIED,          /* 实车目标尚未验证 */
    PK_MSG_FORM_UNVERIFIED,            /* 自制钥匙形态适配未验证 */
    PK_MSG_NEED_APPROVAL,              /* 等待车端批准，不能靠下一步成功 */
    PK_MSG_APPROVED_NEED_AUTH,         /* 车端批准回应已收到，尚需本机认证验证 */
    PK_MSG_PAIR_REJECTED,              /* 车端拒绝或未确认 */
    PK_MSG_PAIR_SAVED,                 /* local pairing record saved */
    PK_MSG_PAIR_VERIFY_FAIL,           /* 验证未完成，车端可能保留登记；请车机检查 */
    PK_MSG_PAIR_SAVE_FAIL,             /* 保存前发现重复或满额，车端可能留登记 */
    PK_MSG_PAIR_TIMEOUT,               /* 配对超时；车端可能留登记，请车机移除 */
    PK_MSG_ORPHAN,                     /* G-04: 车端可能保留登记，请在车机钥匙列表移除 */
    PK_MSG_AP_EXPIRED,                 /* 热点已关闭，请重新进入录入 */
    PK_MSG_AP_VIN_ONLY,                /* 热点只在录入车架号期间开启，不提供独立开关 */
    PK_MSG_RECONNECT_NO_CAR,           /* 未添加车辆 */
    PK_MSG_RECONNECT_IN_VIN,           /* 录入会话中不启动重连扫描 */
    PK_MSG_ALREADY_CONNECTED,          /* 已连接，请检查驻车资格 */
    PK_MSG_RECONNECT_RUNNING,          /* 正在有界重连，不重新计时 */
    PK_MSG_RECONNECT_NO_REPLAY,        /* 重新连接不会补发旧操作 */
    PK_MSG_RECONNECTED,                /* 已连接，挡位／车速仍需检查 */
    PK_MSG_RECONNECT_STOPPED,          /* 自动重连已停止，请手动重连 */
    PK_MSG_RECONNECT_CANCELED,         /* 已停止重连，不补发 */
    PK_MSG_LOW_BATT,                   /* 本机电量低，请充电，仍可闭锁 */
    PK_MSG_STATUS_EXIT,                /* 退出后不后台读取；重进新读 */
    PK_MSG_EDITOR_RANGE,               /* 能力或范围未确认，不可编辑 */
    PK_MSG_EDITOR_INCOMPLETE,          /* 请完成每个参数选择 */
    PK_MSG_SEAT_UNSUPPORTED,           /* 座位或热档不支持 */
    PK_MSG_EDITOR_EXPIRED,             /* 资格已过期，草稿保留；●检查资格，▼取消；不会自动发送 */
    PK_MSG_HOLD_FRUNK,                 /* 继续按住，确认开启；松开▲取消 */
    PK_MSG_HOLD_TRUNK,                 /* 继续按住，确认开启；松开▼取消 */
    PK_MSG_SIDE_NOT_ON_MAIN,           /* 当前页不发送箱盖动作 */
    PK_MSG_LOCAL_ONLY,                 /* 未添加车辆，本机设置仍可用 */
    PK_MSG_STATUS_TIMEOUT,             /* 未读全／超时；不补默认值 */
    PK_MSG_RESPONSE_TOO_LARGE,         /* 车端回包超出蓝牙 MTU；不自动补发 */
    PK_MSG_POWER_DASH,                 /* 功率未验证时显示「—」 */
    PK_MSG_RANGE_UNIT_PENDING,         /* G-01: unit pending; never guess km */
    PK_MSG_BATTERY_UNKNOWN,            /* G-06: unknown is dash, not 0% */
    PK_MSG_REBOOT,                     /* 重启后资格与授权失效 */
    PK_MSG_TRUNK_UNAVAILABLE,          /* driving home: closures unavailable */
    PK_MSG_BLE_SLOTS_FULL,             /* 车已在线，蓝牙3连接额已满（请先关手机蓝牙） */
    PK_MSG_COUNT
} pk_msg_id_t;

const char *pk_copy(pk_msg_id_t id);

/* Single feedback classification for the red failure presentation. */
bool pk_msg_is_failure(pk_msg_id_t id);

/* Formatter for lock-verify result (max two lines). Does not say 全车安全. */
void pk_copy_lock_result(char *out, size_t out_len,
                         pk_msg_id_t lock_msg,
                         const char *closure_part,
                         bool also_unknown);

#ifdef __cplusplus
}
#endif

#endif /* PK_COPY_H */
