#pragma once
#include "nautilus-view-cell.h"
G_BEGIN_DECLS
#define REBUNTU_TYPE_DEVICE_CELL (rebuntu_device_cell_get_type ())
G_DECLARE_FINAL_TYPE (RebuntuDeviceCell, rebuntu_device_cell, REBUNTU, DEVICE_CELL, NautilusViewCell)
NautilusViewCell *rebuntu_device_cell_new (NautilusListBase *view);
G_END_DECLS
