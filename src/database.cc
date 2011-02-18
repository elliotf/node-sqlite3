// Copyright (c) 2010, Orlando Vazquez <ovazquez@gmail.com>
//
// Permission to use, copy, modify, and/or distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#include <string.h>
#include <v8.h>
#include <node.h>
#include <node_events.h>

#include "macros.h"
#include "database.h"
#include "statement.h"
#include "deferred_call.h"

using namespace v8;
using namespace node;


Persistent<FunctionTemplate> Database::constructor_template;

void Database::Init(v8::Handle<Object> target) {
    HandleScope scope;

    Local<FunctionTemplate> t = FunctionTemplate::New(New);

    constructor_template = Persistent<FunctionTemplate>::New(t);
    constructor_template->Inherit(EventEmitter::constructor_template);
    constructor_template->InstanceTemplate()->SetInternalFieldCount(1);
    constructor_template->SetClassName(String::NewSymbol("Database"));

    NODE_SET_PROTOTYPE_METHOD(constructor_template, "close", Close);

    target->Set(v8::String::NewSymbol("Database"),
                constructor_template->GetFunction());
}

void Database::Process(Database* db) {
    if (!db->open && db->locked && !db->queue.empty()) {
        EXCEPTION(String::New("Database is closed"), SQLITE_MISUSE, exception);
        Local<Value> argv[] = { String::NewSymbol("error"), exception };
        EMIT_EVENT(db->handle_, 2, argv);
        return;
    }

    while (db->open && !db->locked && !db->queue.empty()) {
        Call* call = db->queue.front();

        if (call->exclusive && db->pending > 0) {
            break;
        }

        call->callback(call->baton);
        db->queue.pop();
        delete call;
    }
}

inline void Database::Schedule(Database* db, EIO_Callback callback, Baton* baton,
                               bool exclusive = false) {
    if (!db->open && db->locked) {
        EXCEPTION(String::New("Database is closed"), SQLITE_MISUSE, exception);
        if (!(baton)->callback.IsEmpty()) {
            Local<Value> argv[] = { exception };
            TRY_CATCH_CALL(db->handle_, (baton)->callback, 1, argv);
        }
        else {
            Local<Value> argv[] = { String::NewSymbol("error"), exception };
            EMIT_EVENT(db->handle_, 2, argv);
        }
        return;
    }

    if (!db->open || db->locked || (exclusive && db->pending > 0)) {
        db->queue.push(new Call(callback, baton, exclusive));
    }
    else {
        callback(baton);
    }
}

Handle<Value> Database::New(const Arguments& args) {
    HandleScope scope;

    if (!args.IsConstructCall()) {
        return ThrowException(Exception::TypeError(
            String::New("Use the new keyword to create new Database objects"))
        );
    }

    REQUIRE_ARGUMENT_STRING(0, filename);
    int pos = 1;

    int mode = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    if (args.Length() >= pos && args[pos]->IsInt32()) {
        mode = args[pos++]->Int32Value();
    }

    Local<Function> callback;
    if (args.Length() >= pos && args[pos]->IsFunction()) {
        callback = Local<Function>::Cast(args[pos++]);
    }

    Database* db = new Database();
    db->Wrap(args.This());

    args.This()->Set(String::NewSymbol("filename"), args[0]->ToString(), ReadOnly);
    args.This()->Set(String::NewSymbol("mode"), Integer::New(mode), ReadOnly);

    // Start opening the database.
    OpenBaton* baton = new OpenBaton();
    baton->db = db;
    baton->callback = Persistent<Function>::New(callback);
    baton->filename = *filename;
    baton->mode = SQLITE_OPEN_FULLMUTEX | mode;
    EIO_BeginOpen(baton);

    return args.This();
}

void Database::EIO_BeginOpen(Baton* baton) {
    baton->db->Ref();
    ev_ref(EV_DEFAULT_UC);
    eio_custom(EIO_Open, EIO_PRI_DEFAULT, EIO_AfterOpen, baton);
}

int Database::EIO_Open(eio_req *req) {
    OpenBaton* baton = static_cast<OpenBaton*>(req->data);
    Database* db = baton->db;

    baton->status = sqlite3_open_v2(
        baton->filename.c_str(),
        &db->handle,
        baton->mode,
        NULL
    );

    if (baton->status != SQLITE_OK) {
        baton->message = std::string(sqlite3_errmsg(db->handle));
        db->handle = NULL;
    }

    return 0;
}

int Database::EIO_AfterOpen(eio_req *req) {
    HandleScope scope;
    OpenBaton* baton = static_cast<OpenBaton*>(req->data);
    Database* db = baton->db;

    db->Unref();
    ev_unref(EV_DEFAULT_UC);

    Local<Value> argv[1];
    if (baton->status != SQLITE_OK) {
        EXCEPTION(String::New(baton->message.c_str()), baton->status, exception);
        argv[0] = exception;
    }
    else {
        db->open = true;
        argv[0] = Local<Value>::New(Null());
    }

    if (!baton->callback.IsEmpty()) {
        TRY_CATCH_CALL(db->handle_, baton->callback, 1, argv);
    }
    else if (!db->open) {
        Local<Value> args[] = { String::NewSymbol("error"), argv[0] };
        EMIT_EVENT(db->handle_, 2, args);
    }

    if (db->open) {
        Local<Value> args[] = { String::NewSymbol("open") };
        EMIT_EVENT(db->handle_, 1, args);
        Process(db);
    }

    delete baton;
    return 0;
}

Handle<Value> Database::Close(const Arguments& args) {
    HandleScope scope;
    Database* db = ObjectWrap::Unwrap<Database>(args.This());
    OPTIONAL_ARGUMENT_FUNCTION(0, callback);

    db->Ref();
    ev_ref(EV_DEFAULT_UC);

    Baton* baton = new Baton();
    baton->db = db;
    baton->callback = Persistent<Function>::New(callback);
    Schedule(db, EIO_BeginClose, baton, true);

    return args.This();
}

void Database::EIO_BeginClose(Baton* baton) {
    assert(baton->db->open);
    assert(!baton->db->locked);
    assert(baton->db->pending == 0);
    baton->db->locked = true;
    eio_custom(EIO_Close, EIO_PRI_DEFAULT, EIO_AfterClose, baton);
}

int Database::EIO_Close(eio_req *req) {
    Baton* baton = static_cast<Baton*>(req->data);
    Database* db = baton->db;

    baton->status = sqlite3_close(db->handle);

    if (baton->status != SQLITE_OK) {
        baton->message = std::string(sqlite3_errmsg(db->handle));
    }
    else {
        db->handle = NULL;
    }
    return 0;
}

int Database::EIO_AfterClose(eio_req *req) {
    HandleScope scope;
    Baton* baton = static_cast<Baton*>(req->data);
    Database* db = baton->db;

    ev_unref(EV_DEFAULT_UC);
    db->Unref();

    Local<Value> argv[1];
    if (baton->status != SQLITE_OK) {
        EXCEPTION(String::New(baton->message.c_str()), baton->status, exception);
        argv[0] = exception;
    }
    else {
        db->open = false;
        // Leave db->locked to indicate that this db object has reached
        // the end of its life.
        argv[0] = Local<Value>::New(Null());
    }

    // Fire callbacks.
    if (!baton->callback.IsEmpty()) {
        TRY_CATCH_CALL(db->handle_, baton->callback, 1, argv);
    }
    else if (db->open) {
        Local<Value> args[] = { String::NewSymbol("error"), argv[0] };
        EMIT_EVENT(db->handle_, 2, args);
    }

    if (!db->open) {
        Local<Value> args[] = { String::NewSymbol("close"), argv[0] };
        EMIT_EVENT(db->handle_, 1, args);
        Process(db);
    }

    delete baton;

    return 0;
}

/**
 * Override this so that we can properly close the database when this object
 * gets garbage collected.
 */
void Database::Wrap(Handle<Object> handle) {
    assert(handle_.IsEmpty());
    assert(handle->InternalFieldCount() > 0);
    handle_ = Persistent<Object>::New(handle);
    handle_->SetPointerInInternalField(0, this);
    handle_.MakeWeak(this, Destruct);
}

inline void Database::MakeWeak (void) {
    handle_.MakeWeak(this, Destruct);
}

void Database::Unref() {
    assert(!handle_.IsEmpty());
    assert(!handle_.IsWeak());
    assert(refs_ > 0);
    if (--refs_ == 0) { MakeWeak(); }
}

void Database::Destruct(Persistent<Value> value, void *data) {
    Database* db = static_cast<Database*>(data);
    if (db->handle) {
        eio_custom(EIO_Destruct, EIO_PRI_DEFAULT, EIO_AfterDestruct, db);
        ev_ref(EV_DEFAULT_UC);
    }
    else {
        delete db;
    }
}

int Database::EIO_Destruct(eio_req *req) {
    Database* db = static_cast<Database*>(req->data);

    sqlite3_close(db->handle);
    db->handle = NULL;

    return 0;
}

int Database::EIO_AfterDestruct(eio_req *req) {
    Database* db = static_cast<Database*>(req->data);
    ev_unref(EV_DEFAULT_UC);
    delete db;
    return 0;
}
