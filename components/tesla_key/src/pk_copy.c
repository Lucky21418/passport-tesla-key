#include "pk_copy.h"

#include <stdio.h>
#include <string.h>

/*
 * String table for the application layer. Official Stitch pages are not
 * edited here (t807u / t816u). Wording polish belongs to
 * contract wording; keep safety meanings intact. Contract-mandated meaning is kept.
 *
 * Not a copy task: main-screen vehicle identity (t816u, discarded).
 * Do not reintroduce 示例·非实车 as production chrome (A-01).
 */

static const char *const k_copy[PK_MSG_COUNT] = {
    [PK_MSG_EMPTY] = "",
    [PK_MSG_PRESSED] = "已按下●，等待手势",
    [PK_MSG_CANCELLED] = "已取消，未发送",
    [PK_MSG_BUSY_NO_QUEUE] = "处理中，不排队",
    [PK_MSG_RECONNECT_FIRST] = "暂未连上车，请先重连",
    [PK_MSG_NO_TARGET] = "未添加车辆",
    [PK_MSG_NOT_PAIRED] = "尚未完成配对",
    [PK_MSG_UNQUALIFIED] = "当前不可用",
    [PK_MSG_MOVING] = "行驶中不可用",
    [PK_MSG_CAPABILITY] = "本功能暂不支持或未确认",
    [PK_MSG_LOCK_SNAP_BLOCK] = "锁状态未知，暂不可反向开锁",
    [PK_MSG_CABLE] = "请先断开充电枪再关充电口",
    [PK_MSG_CAR_WASH] = "洗车模式中，暂不可操作充电口",
    [PK_MSG_NEED_CLIMATE] = "请先单独开启空调，再操作",
    [PK_MSG_OLD_GESTURE] = "旧手势已清除，请松开后重新操作",
    [PK_MSG_SENT_WAIT] = "已发送，等待车辆响应",
    [PK_MSG_HOLD_DANGER] = "长按执行",
    [PK_MSG_STATUS_READONLY] = "状态页仅查看，不发送车控",
    [PK_MSG_DRV_MANAGEMENT] = "行驶中不可进入车辆管理",
    [PK_MSG_GRANT_EXPIRED] = "当前不可用",
    [PK_MSG_QUAL_IN_PROGRESS] = "处理中",
    [PK_MSG_QUAL_OK] = "已完成",
    [PK_MSG_QUAL_MOVING] = "行驶中不可用",
    [PK_MSG_WAKE_OFFER] = "车辆可能已休眠，●唤醒，▼返回",
    [PK_MSG_WAKE_OFFER_UNKNOWN] = "挡位／车速未确认；可用原厂卡",
    [PK_MSG_WAKE_AGAIN] = "读取不足，车可能休眠",
    [PK_MSG_WAKE_DECLINED] = "已返回，未唤醒，不补发旧操作",
    [PK_MSG_WAKE_FAIL] = "唤醒未确认，请用原厂卡",
    [PK_MSG_WAKE_TIMEOUT] = "唤醒超时，请用原厂卡重连",
    [PK_MSG_QUAL_NEED] = "无车辆／请先重连／处理中",
    [PK_MSG_WAIT_EXIT] = "已退出等待，已发指令不撤销",
    [PK_MSG_CMD_WAIT_MID] = "车辆处理中，请等待",
    [PK_MSG_CMD_UNRECOGNIZED] = "响应未识别，继续等待",
    [PK_MSG_CMD_ACCEPTED] = "操作已执行，请查看状态",
    [PK_MSG_CMD_REJECTED] = "车辆已拒绝，未自动重发",
    [PK_MSG_CMD_UNCONFIRMED] = "结果未确认，请现场查看",
    [PK_MSG_CMD_TIMEOUT] = "响应超时，请现场查看；不重发",
    /* G-03: do not write 已确认可挂挡; 60 s is a local notice only. */
    [PK_MSG_AUTH_REJECTED] = "授权请求被拒绝，请检查车内设置／PIN",
    [PK_MSG_AUTH_UNCONFIRMED] = "授权请求未确认，请车内确认",
    [PK_MSG_AUTH_EXPIRED] = "授权请求提示已过期，请车内确认",
    [PK_MSG_LOCK_VERIFYING] = "正在确认锁车...",
    /* G-05: never claim 全车安全. */
    [PK_MSG_LOCK_CONFIRMED] = "已锁车",
    [PK_MSG_LOCK_UNLOCKED] = "当前未锁定",
    [PK_MSG_LOCK_UNCONFIRMED] = "未收到锁车确认",
    [PK_MSG_LOCK_NO_LINK] = "锁车未确认，车可能已锁，请现场确认",
    [PK_MSG_CLOSURES_MANY] = "多处门盖未关",
    [PK_MSG_CLOSURES_SOME_UNKNOWN] = "门盖状态未返回",
    [PK_MSG_MGMT_BUSY_OR_MOVING] = "行驶中或处理中，不可管理车辆",
    [PK_MSG_MGMT_CONNECTED_UNKNOWN] = "当前状态不能断开驾驶连接",
    [PK_MSG_SWITCHED] = "已切换车辆，正在重新连接",
    [PK_MSG_RECORD_UPDATED] = "本机记录已更新；车端登记未撤销",
    [PK_MSG_DELETE_INCOMPLETE] = "删除未完成，保留旧配置",
    [PK_MSG_DELETE_RECOVERED] = "已恢复提交后的本机配置；车端登记未撤销",
    [PK_MSG_FULL_OR_PROTECTED] = "车辆已满或当前驾驶会话受保护",
    [PK_MSG_VIN_OK] = "车架号已录入，请核对后配对",
    [PK_MSG_VIN_BAD] = "格式／重复／容量错误，请重新录入",
    [PK_MSG_TARGET_UNVERIFIED] = "实车目标尚未验证",
    [PK_MSG_FORM_UNVERIFIED] = "自制钥匙形态适配未验证",
    [PK_MSG_NEED_APPROVAL] = "请先放原厂卡读卡区授权",
    [PK_MSG_APPROVED_NEED_AUTH] = "车端已批准，按●完成验证",
    [PK_MSG_PAIR_REJECTED] = "车端拒绝或未确认",
    [PK_MSG_PAIR_SAVED] = "配对成功，钥匙已保存",
    [PK_MSG_PAIR_VERIFY_FAIL] = "验证未通过，请在车机移除后重配",
    [PK_MSG_PAIR_SAVE_FAIL] = "保存前发现重复或满额，车端可能留登记",
    [PK_MSG_PAIR_TIMEOUT] = "配对超时，请检查车机钥匙列表",
    /* G-04: keep "本机记录, 不撤销车端登记". */
    [PK_MSG_ORPHAN] = "已取消，车机留有登记请手动移除",
    [PK_MSG_AP_EXPIRED] = "热点已关闭，请重新进入录入",
    [PK_MSG_AP_VIN_ONLY] = "热点只在录入车架号期间开启，不提供独立开关",
    [PK_MSG_RECONNECT_NO_CAR] = "未添加车辆",
    [PK_MSG_RECONNECT_IN_VIN] = "录入会话中不启动重连扫描",
    [PK_MSG_ALREADY_CONNECTED] = "已连接，可正常控车",
    [PK_MSG_RECONNECT_RUNNING] = "正在重连车辆，请等待",
    [PK_MSG_RECONNECT_NO_REPLAY] = "重连中，不会补发旧指令",
    [PK_MSG_RECONNECTED] = "已连上车，按●锁车或双击查状态",
    [PK_MSG_RECONNECT_STOPPED] = "暂未连上车，按●重连",
    [PK_MSG_RECONNECT_CANCELED] = "已停止重连，不补发",
    [PK_MSG_LOW_BATT] = "本机电量低，请充电，仍可锁车／开锁／启动",
    [PK_MSG_STATUS_EXIT] = "已退出状态页",
    [PK_MSG_EDITOR_RANGE] = "能力或范围未确认，不可编辑",
    [PK_MSG_EDITOR_INCOMPLETE] = "请完成每个参数选择",
    [PK_MSG_SEAT_UNSUPPORTED] = "座位或热档不支持",
    [PK_MSG_EDITOR_EXPIRED] = "当前不可用，草稿已保留",
    [PK_MSG_HOLD_FRUNK] = "继续按住，确认开启；松开▲取消",
    [PK_MSG_HOLD_TRUNK] = "继续按住，确认开启；松开▼取消",
    [PK_MSG_SIDE_NOT_ON_MAIN] = "当前页不发送箱盖动作",
    [PK_MSG_LOCAL_ONLY] = "未添加车辆，本机设置仍可用",
    [PK_MSG_STATUS_TIMEOUT] = "车辆可能休眠，按●重读",
    /* G-01: honest pending units; never invent km / kW. */
    [PK_MSG_POWER_DASH] = "—",
    [PK_MSG_RANGE_UNIT_PENDING] = "单位待确认",
    /* G-06: unknown battery is "—", never "0%". */
    [PK_MSG_BATTERY_UNKNOWN] = "—",
    [PK_MSG_REBOOT] = "重启后资格与授权失效",
    /* Driving home: place the closure note in the subtitle. Do not open a
     * driving-writable defog entry or change the long-● mapping.
     */
    [PK_MSG_TRUNK_UNAVAILABLE] = "箱盖不可用；除雾请用车内控制",
    [PK_MSG_BLE_SLOTS_FULL] = "车已在线，蓝牙3连接额已满（请先关手机蓝牙）",
    [PK_MSG_RESPONSE_TOO_LARGE] = "车辆响应过大，未补发",
};

const char *pk_copy(pk_msg_id_t id)
{
    if (id <= PK_MSG_EMPTY || id >= PK_MSG_COUNT || !k_copy[id]) {
        return "";
    }
    return k_copy[id];
}

bool pk_msg_is_failure(pk_msg_id_t id)
{
    switch (id) {
    case PK_MSG_CMD_REJECTED:
    case PK_MSG_CMD_TIMEOUT:
    case PK_MSG_CMD_UNCONFIRMED:
    case PK_MSG_LOCK_UNCONFIRMED:
    case PK_MSG_LOCK_NO_LINK:
    case PK_MSG_LOCK_UNLOCKED:
    case PK_MSG_WAKE_FAIL:
    case PK_MSG_WAKE_TIMEOUT:
    case PK_MSG_AUTH_REJECTED:
    case PK_MSG_AUTH_UNCONFIRMED:
    case PK_MSG_NEED_APPROVAL:
    case PK_MSG_RESPONSE_TOO_LARGE:
        return true;
    default:
        return false;
    }
}

void pk_copy_lock_result(char *out, size_t out_len,
                         pk_msg_id_t lock_msg,
                         const char *closure_part,
                         bool also_unknown)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    snprintf(out, out_len, "%s", pk_copy(lock_msg));
    if (closure_part && closure_part[0]) {
        strncat(out, "；", out_len - strlen(out) - 1);
        strncat(out, closure_part, out_len - strlen(out) - 1);
    }
    if (also_unknown) {
        strncat(out, "；", out_len - strlen(out) - 1);
        strncat(out, pk_copy(PK_MSG_CLOSURES_SOME_UNKNOWN), out_len - strlen(out) - 1);
    }
}
