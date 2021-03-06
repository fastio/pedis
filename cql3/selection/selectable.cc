/*
 * Copyright (C) 2015 ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "selectable.hh"
#include "selectable_with_field_selection.hh"
#include "field_selector.hh"
#include "writetime_or_ttl.hh"
#include "selector_factories.hh"
#include "cql3/functions/functions.hh"
#include "cql3/functions/castas_fcts.hh"
#include "cql3/functions/aggregate_fcts.hh"
#include "abstract_function_selector.hh"
#include "writetime_or_ttl_selector.hh"

namespace cql3 {

namespace selection {

shared_ptr<selector::factory>
selectable::writetime_or_ttl::new_selector_factory(database& db, schema_ptr s, std::vector<const column_definition*>& defs) {
    auto&& def = s->get_column_definition(_id->name());
    if (!def) {
        throw exceptions::invalid_request_exception(sprint("Undefined name %s in selection clause", _id));
    }
    if (def->is_primary_key()) {
        throw exceptions::invalid_request_exception(
                sprint("Cannot use selection function %s on PRIMARY KEY part %s",
                              _is_writetime ? "writeTime" : "ttl",
                              def->name()));
    }
    if (def->type->is_collection()) {
        throw exceptions::invalid_request_exception(sprint("Cannot use selection function %s on collections",
                                                        _is_writetime ? "writeTime" : "ttl"));
    }

    return writetime_or_ttl_selector::new_factory(def->name_as_text(), add_and_get_index(*def, defs), _is_writetime);
}

sstring
selectable::writetime_or_ttl::to_string() const {
    return sprint("%s(%s)", _is_writetime ? "writetime" : "ttl", _id->to_string());
}

shared_ptr<selectable>
selectable::writetime_or_ttl::raw::prepare(schema_ptr s) {
    return make_shared<writetime_or_ttl>(_id->prepare_column_identifier(s), _is_writetime);
}

bool
selectable::writetime_or_ttl::raw::processes_selection() const {
    return true;
}

shared_ptr<selector::factory>
selectable::with_function::new_selector_factory(database& db, schema_ptr s, std::vector<const column_definition*>& defs) {
    auto&& factories = selector_factories::create_factories_and_collect_column_definitions(_args, db, s, defs);

    // resolve built-in functions before user defined functions
    auto&& fun = functions::functions::get(db, s->ks_name(), _function_name, factories->new_instances(), s->ks_name(), s->cf_name());
    if (!fun) {
        throw exceptions::invalid_request_exception(sprint("Unknown function '%s'", _function_name));
    }
    if (!fun->return_type()) {
        throw exceptions::invalid_request_exception(sprint("Unknown function %s called in selection clause", _function_name));
    }

    return abstract_function_selector::new_factory(std::move(fun), std::move(factories));
}

sstring
selectable::with_function::to_string() const {
    return sprint("%s(%s)", _function_name.name, join(", ", _args));
}

shared_ptr<selectable>
selectable::with_function::raw::prepare(schema_ptr s) {
        std::vector<shared_ptr<selectable>> prepared_args;
        prepared_args.reserve(_args.size());
        for (auto&& arg : _args) {
            prepared_args.push_back(arg->prepare(s));
        }
        return ::make_shared<with_function>(_function_name, std::move(prepared_args));
    }

bool
selectable::with_function::raw::processes_selection() const {
    return true;
}

shared_ptr<selectable::with_function::raw>
selectable::with_function::raw::make_count_rows_function() {
    return ::make_shared<cql3::selection::selectable::with_function::raw>(
            cql3::functions::function_name::native_function(cql3::functions::aggregate_fcts::COUNT_ROWS_FUNCTION_NAME),
                    std::vector<shared_ptr<cql3::selection::selectable::raw>>());
}

shared_ptr<selector::factory>
selectable::with_anonymous_function::new_selector_factory(database& db, schema_ptr s, std::vector<const column_definition*>& defs) {
    auto&& factories = selector_factories::create_factories_and_collect_column_definitions(_args, db, s, defs);
    return abstract_function_selector::new_factory(_function, std::move(factories));
}

sstring
selectable::with_anonymous_function::to_string() const {
    return sprint("%s(%s)", _function->name().name, join(", ", _args));
}

shared_ptr<selectable>
selectable::with_anonymous_function::raw::prepare(schema_ptr s) {
        std::vector<shared_ptr<selectable>> prepared_args;
        prepared_args.reserve(_args.size());
        for (auto&& arg : _args) {
            prepared_args.push_back(arg->prepare(s));
        }
        return ::make_shared<with_anonymous_function>(_function, std::move(prepared_args));
    }

bool
selectable::with_anonymous_function::raw::processes_selection() const {
    return true;
}

shared_ptr<selector::factory>
selectable::with_field_selection::new_selector_factory(database& db, schema_ptr s, std::vector<const column_definition*>& defs) {
    auto&& factory = _selected->new_selector_factory(db, s, defs);
    auto&& type = factory->new_instance()->get_type();
    auto&& ut = dynamic_pointer_cast<const user_type_impl>(std::move(type));
    if (!ut) {
        throw exceptions::invalid_request_exception(
                sprint("Invalid field selection: %s of type %s is not a user type",
                       _selected->to_string(), factory->new_instance()->get_type()->as_cql3_type()));
    }
    for (size_t i = 0; i < ut->size(); ++i) {
        if (ut->field_name(i) != _field->bytes_) {
            continue;
        }
        return field_selector::new_factory(std::move(ut), i, std::move(factory));
    }
    throw exceptions::invalid_request_exception(sprint("%s of type %s has no field %s",
                                                       _selected->to_string(), ut->as_cql3_type(), _field));
}

sstring
selectable::with_field_selection::to_string() const {
    return sprint("%s.%s", _selected->to_string(), _field->to_string());
}

shared_ptr<selectable>
selectable::with_field_selection::raw::prepare(schema_ptr s) {
    // static_pointer_cast<> needed due to lack of covariant return type
    // support with smart pointers
    return make_shared<with_field_selection>(_selected->prepare(s),
            static_pointer_cast<column_identifier>(_field->prepare(s)));
}

bool
selectable::with_field_selection::raw::processes_selection() const {
    return true;
}

shared_ptr<selector::factory>
selectable::with_cast::new_selector_factory(database& db, schema_ptr s, std::vector<const column_definition*>& defs) {
    std::vector<shared_ptr<selectable>> args{_arg};
    auto&& factories = selector_factories::create_factories_and_collect_column_definitions(args, db, s, defs);
    auto&& fun = functions::castas_functions::get(_type->get_type(), factories->new_instances(), s);

    return abstract_function_selector::new_factory(std::move(fun), std::move(factories));
}

sstring
selectable::with_cast::to_string() const {
    return sprint("cast(%s as %s)", _arg->to_string(), _type->to_string());
}

shared_ptr<selectable>
selectable::with_cast::raw::prepare(schema_ptr s) {
    return ::make_shared<selectable::with_cast>(_arg->prepare(s), _type);
}

bool
selectable::with_cast::raw::processes_selection() const {
    return true;
}

std::ostream & operator<<(std::ostream &os, const selectable& s) {
    return os << s.to_string();
}

}

}
