#pragma once

enum AccessPermission {
    AccessPermission_Invalid,
    AccessPermission_NotPresent,
    AccessPermission_Busy,
    AccessPermission_Read_Only,
    AccessPermission_Read_Write,
    AccessPermission_Maybe_Stale,
    AccessPermission_Backing_Store,
    AccessPermission_NUM
};
