/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/kernel.h>
#include <kernel/mm.h>
#include "board.h"

extern addr_t __mm_pool_start, __mm_pool_end;
extern addr_t __mm_sys_reserve_start, __mm_sys_reserve_end;

mm_reserve_node_register((addr_t)&__mm_sys_reserve_start, (addr_t)&__mm_sys_reserve_end, reserve);
mm_node_register((addr_t)&__mm_pool_start, (addr_t)&__mm_pool_end, inside);

void board_mm_init(void)
{
}
