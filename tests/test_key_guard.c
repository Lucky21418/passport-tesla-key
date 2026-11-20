// 按键唤醒守卫状态机的 host 测试:不依赖 SDK,直接验证 key_guard.h 的纯逻辑。
#include <assert.h>
#include <stdio.h>

#include "key_guard.h"

int main(void) {
    key_guard_t g = {0};

    // 未武装:不吞事件,也不改状态。
    assert(!key_guard_consume(&g, false, 0));
    assert(!g.armed);

    // 武装后:普通事件被吞,直到松手或到期。
    key_guard_arm(&g, 1000, 10000);
    assert(g.armed);
    assert(key_guard_consume(&g, false, 1500));   // 长按被吞
    assert(g.armed);
    assert(key_guard_consume(&g, false, 2500));   // 仍被吞
    assert(g.armed);

    // 松手事件撤防,且该事件本身也被吞。
    assert(key_guard_consume(&g, true, 2600));
    assert(!g.armed);
    assert(!key_guard_consume(&g, false, 2700));  // 之后正常派发

    // 到期兜底:没有松手事件也能撤防。
    key_guard_arm(&g, 1000, 10000);               // deadline = 11000
    assert(key_guard_consume(&g, false, 11001));  // 到期,吞掉这次
    assert(!g.armed);
    assert(!key_guard_consume(&g, false, 11002));

    // 释放早于初始化:按键驱动启动前就已松手,不会再有 release 事件。
    // 按采样到的物理状态(已松手)同步后立即撤防,下一次长按不再被吞。
    key_guard_arm(&g, 1000, 10000);
    key_guard_sync_released(&g, true);
    assert(!g.armed);
    assert(!key_guard_consume(&g, false, 1500));  // 下一次长按正常派发

    // 按住贯穿初始化:采样仍按住,守卫保持武装,直到松手事件。
    key_guard_arm(&g, 1000, 10000);
    key_guard_sync_released(&g, false);           // 仍按住,不撤防
    assert(g.armed);
    assert(key_guard_consume(&g, false, 1500));   // 长按仍被吞
    assert(g.armed);
    assert(key_guard_consume(&g, true, 2600));    // 松手撤防
    assert(!g.armed);

    /* Stuck key: 10s boundary unchanged. Release asks the veto and does not
     * deep-sleep by itself. Held states still pump events and suppress new edges. */
    {
        key_stuck_guard_t st = {0};
        unsigned held;
        unsigned released;
        assert(key_stuck_update(&st, true, 1000, 10000) == KEY_STUCK_NONE);
        assert(key_stuck_update(&st, true, 11000, 10000) == KEY_STUCK_NONE); /* exactly 10s */
        assert(key_stuck_update(&st, true, 11001, 10000) == KEY_STUCK_ENTER);
        held = key_stuck_loop_policy(KEY_STUCK_ENTER);
        assert((held & KEY_STUCK_POLICY_PUMP) != 0);
        assert((held & KEY_STUCK_POLICY_SUPPRESS_NEW) != 0);
        assert((held & KEY_STUCK_POLICY_DIRECT_SLEEP) == 0);
        assert(key_stuck_update(&st, true, 11200, 10000) == KEY_STUCK_WAITING);
        assert(key_stuck_update(&st, false, 11300, 10000) == KEY_STUCK_RELEASED);
        released = key_stuck_loop_policy(KEY_STUCK_RELEASED);
        assert((released & KEY_STUCK_POLICY_PUMP) != 0);
        assert((released & KEY_STUCK_POLICY_ASK_VETO) != 0);
        assert((released & KEY_STUCK_POLICY_SUPPRESS_NEW) != 0);
        assert((released & KEY_STUCK_POLICY_DIRECT_SLEEP) == 0);
        assert(key_stuck_update(&st, false, 11400, 10000) == KEY_STUCK_NONE);
        assert((key_stuck_loop_policy(KEY_STUCK_NONE) & KEY_STUCK_POLICY_DIRECT_SLEEP) == 0);
    }

    puts("key wake guard state-machine tests: PASS");
    return 0;
}
