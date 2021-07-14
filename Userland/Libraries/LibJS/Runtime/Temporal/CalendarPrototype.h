/*
 * Copyright (c) 2021, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Object.h>

namespace JS::Temporal {

class CalendarPrototype final : public Object {
    JS_OBJECT(CalendarPrototype, Object);

public:
    explicit CalendarPrototype(GlobalObject&);
    virtual void initialize(GlobalObject&) override;
    virtual ~CalendarPrototype() override = default;
};

}
