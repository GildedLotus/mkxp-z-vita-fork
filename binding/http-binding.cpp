//
//  http-binding.cpp
//  mkxp-z
//
//  Created by ゾロアーク on 12/29/20.
//

#include <stdio.h>

#include "util/json5pp.hpp"
#include "binding-util.h"

// The player does not initialize sceNet; reject before parsing or dispatching.
RB_METHOD(httpUnavailable) {
    RB_UNUSED_PARAM;
    rb_raise(rb_eNotImpError, "HTTPLite networking is not supported on PS Vita");
    return Qnil;
}

VALUE json2rb(json5pp::value const &v) {
    if (v.is_null())
        return Qnil;
    
    if (v.is_number())
        return rb_float_new(v.as_number());
    
    if (v.is_string())
        return rb_utf8_str_new_cstr(v.as_string().c_str());
    
    if (v.is_boolean())
        return rb_bool_new(v.as_boolean());
    
    if (v.is_integer())
        return LL2NUM(v.as_integer());
    
    if (v.is_array()) {
        auto &a = v.as_array();
        VALUE ret = rb_ary_new();
        for (auto item : a) {
            rb_ary_push(ret, json2rb(item));
        }
        return ret;
    }
    
    if (v.is_object()) {
        auto &o = v.as_object();
        VALUE ret = rb_hash_new();
        for (auto const &pair : o) {
            rb_hash_aset(ret, rb_utf8_str_new_cstr(pair.first.c_str()), json2rb(pair.second));
        }
        return ret;
    }
    
    // This should be unreachable
    return Qnil;
}

json5pp::value rb2json(VALUE v) {
    if (v == Qnil)
        return json5pp::value(nullptr);
    
    if (RB_TYPE_P(v, RUBY_T_FLOAT))
        return json5pp::value(RFLOAT_VALUE(v));
    
    if (RB_TYPE_P(v, RUBY_T_STRING))
        return json5pp::value(RSTRING_PTR(v));
    
    if (v == Qtrue || v == Qfalse)
        return json5pp::value(RTEST(v));
    
    if (RB_TYPE_P(v, RUBY_T_FIXNUM))
        return json5pp::value(NUM2DBL(v));
    
    if (RB_TYPE_P(v, RUBY_T_ARRAY)) {
        json5pp::value ret_value = json5pp::array({});
        auto &ret = ret_value.as_array();
        for (int i = 0; i < RARRAY_LEN(v); i++) {
            ret.push_back(rb2json(rb_ary_entry(v, i)));
        }
        return ret_value;
    }
    
    if (RTEST(rb_funcall(v, rb_intern("is_a?"), 1, rb_cHash))) {
        json5pp::value ret_value = json5pp::object({});
        auto &ret = ret_value.as_object();
        
        
        VALUE keys = rb_funcall(v, rb_intern("keys"), 0);
        
        for (int i = 0; i < RARRAY_LEN(keys); i++) {
            VALUE key = rb_ary_entry(keys, i); SafeStringValue(key);
            VALUE val = rb_hash_aref(v, key);
            ret.emplace(RSTRING_PTR(key), rb2json(val));
        }
        
        return ret_value;
    }
    
    throw Exception(Exception::MKXPError, "Invalid value for JSON: %s", RSTRING_PTR(rb_inspect(v)));
    
    // This should be unreachable
    return json5pp::value(0);
}

RB_METHOD_GUARD(httpJsonParse) {
    RB_UNUSED_PARAM;
    
    VALUE jsonv;
    rb_scan_args(argc, argv, "1", &jsonv);
    SafeStringValue(jsonv);
    
    json5pp::value v;
    try {
        v = json5pp::parse5(RSTRING_PTR(jsonv));
    }
    catch (const std::exception &e) {
        throw Exception(Exception::MKXPError, "Failed to parse JSON: %s", e.what());
    }
    
    return json2rb(v);
}
RB_METHOD_GUARD_END

RB_METHOD_GUARD(httpJsonStringify) {
    RB_UNUSED_PARAM;
    
    VALUE obj;
    rb_scan_args(argc, argv, "1", &obj);
    
    json5pp::value v = rb2json(obj);
    return rb_utf8_str_new_cstr(v.stringify5(json5pp::rule::space_indent<>()).c_str());
}
RB_METHOD_GUARD_END

void httpBindingInit() {
    VALUE mNet = rb_define_module("HTTPLite");
    _rb_define_module_function(mNet, "get", httpUnavailable);
    _rb_define_module_function(mNet, "post", httpUnavailable);
    _rb_define_module_function(mNet, "post_body", httpUnavailable);
    
    VALUE mNetJSON = rb_define_module_under(mNet, "JSON");
    _rb_define_module_function(mNetJSON, "stringify", httpJsonStringify);
    _rb_define_module_function(mNetJSON, "parse", httpJsonParse);
}
