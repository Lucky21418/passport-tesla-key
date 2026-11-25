#ifndef PK_POLICY_H
#define PK_POLICY_H

/*
 * Product-initial policy constants from contracts.py POLICY.
 * These are NOT measured safety TTLs, haptic conclusions, or endurance
 * results. Do not change the numbers without a contract revision.
 * 所有【待真机】门禁在未适配前不得把这些初值写成已测保证。
 */

#define PK_DOUBLE_MS                 300u
#define PK_MAIN_HOLD_MS              500u
#define PK_DANGER_HOLD_MS            500u
#define PK_QUALIFICATION_READ_MS     4000u
#define PK_WAKE_THEN_READ_MS         16000u
/* Settle delay between a confirmed VCSEC wake and the command, from the
 * field-proven tesla-local-control pattern (7 s). Not a safety TTL. */
#define PK_WAKE_SETTLE_MS             7000u
#define PK_PARK_LEASE_MS             60000u
/* A park proof older than this is re-read before any park-gated command. */
#define PK_PARK_FRESH_MS             1000u
#define PK_LOCK_LEASE_MS             60000u
#define PK_CLOCK_FUTURE_TOLERANCE_MS 2000u
#define PK_COMMAND_WAIT_MS           5000u  /* M6 命令阶段有限超时，必须列明；不得无限等待 */
#define PK_LOCK_VERIFY_MS            3000u  /* T_VERIFY 硬限，从命令终结进入 VERIFY 起算，不续命 */
#define PK_LOCK_RESULT_AUTO_RETURN_MS 2000u /* 锁车/开锁成功确认后自动回主页窗口 */
#define PK_AUTHORIZATION_NOTICE_MS   60000u /* 本机提示寿命，不是车端授权期限 */
#define PK_STATUS_READ_MS            4000u
#define PK_STATUS_DISPLAY_MS         15000u
#define PK_AP_INACTIVE_MS            120000u /* VIN 热点绝对上限，与 30s 息屏分开 */
#define PK_PAIR_APPROVAL_MS          60000u
#define PK_PAIR_VERIFY_MS            25000u
#define PK_SCREEN_MS                 30000u
#define PK_LOW_BATT_SCREEN_MS        5000u  /* <=5% 低电应急执行后快速回睡窗口 */
#define PK_LOCK_ALERT_DISPLAY_MS     15000u /* 锁车失败/门盖未关红底醒屏警告窗口 */
#define PK_STUCK_KEY_MS              10000u /* >10s GPIO0 连续按压判定为兜内卡键 */
#define PK_LOG_CAPACITY              64u

/* Reconnect budgets (C11). Product initials, 待整机验证. */
#define PK_RECONNECT_SCAN_MS         10000u  /* advertisement arrives ~every 9 s (sporadic); one window must cover it */
#define PK_RECONNECT_COMFORT_MAX     3u
#define PK_RECONNECT_COMFORT_GAP1_MS 2000u
#define PK_RECONNECT_COMFORT_GAP2_MS 5000u
#define PK_RECONNECT_COMFORT_TOTAL_MS 20000u
#define PK_RECONNECT_ENDURANCE_MAX   2u
#define PK_RECONNECT_ENDURANCE_GAP_MS 5000u
#define PK_RECONNECT_ENDURANCE_TOTAL_MS 15000u

/* Power-button product initials. 待真机校准. */
#define PK_POWER_ON_HOLD_MS          500u
#define PK_POWER_OFF_HOLD_MS         2000u

/* Hardware snapshot (specification, not a measured result of this app). */
#define PK_DISPLAY_WIDTH             240
#define PK_DISPLAY_HEIGHT            320
#define PK_CHROME_TOP_PX             22
#define PK_CHROME_BOTTOM_PX          36
#define PK_FONT_MIN_PX               12
#define PK_MAX_VEHICLES              3
#define PK_BRIGHTNESS_DEFAULT        60
#define PK_BRIGHTNESS_STEP           20
#define PK_BRIGHTNESS_MIN            20
#define PK_BRIGHTNESS_MAX            100

/* BSP voltage-lock delays: aligned to official BSP 40 ms press confirm and
 * 120 ms cross-window / side-key hold (P1 adjudication), while clean ● release
 * into the >= 1900 mV idle band uses symmetric 40 ms confirmation so rapid
 * double-taps are not merged or inflated by +80 ms.
 */
#define PK_BSP_BTN_CONFIRM_MS        40u
#define PK_BSP_BTN_HOLD_MS           120u
#define PK_BTN_DOT_RELEASE_MS        40u

/* Raw-edge timing: 300 ms product double-click window, 500 ms long press.
 * The app does not consume cooked BSP events; the adapter feeds raw edges and
 * this state machine applies the product contract.
 */

#endif /* PK_POLICY_H */
