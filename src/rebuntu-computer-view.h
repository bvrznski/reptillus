/*
 * Copyright (C) 2024 The GNOME project contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "nautilus-list-base.h"

G_BEGIN_DECLS

#define REBUNTU_TYPE_COMPUTER_VIEW (rebuntu_computer_view_get_type())

G_DECLARE_FINAL_TYPE (RebuntuComputerView, rebuntu_computer_view, REBUNTU, COMPUTER_VIEW, NautilusListBase)

RebuntuComputerView *rebuntu_computer_view_new (void);

G_END_DECLS
