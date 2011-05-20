#include <v8.h>
#include <node.h>
#include <node_events.h>

#include <stdlib.h>
#include <errno.h>

#include <config.h>
#include <radiusclient-ng.h>

using namespace node;
using namespace v8;

#define REQ_FUN_ARG(I, VAR)                                             \
  if (args.Length() <= (I) || !args[I]->IsFunction())                   \
    return ThrowException(Exception::TypeError(                         \
                  String::New("Argument " #I " must be a function")));  \
  Local<Function> VAR = Local<Function>::Cast(args[I]);


class Radius: ObjectWrap
{
private:
  rc_handle	*rh;
  VALUE_PAIR 	*send, *received;
  char 		msg[4096];
  bool          busy;

  struct RadiusRequest {
    Radius * r;
    Persistent<Function> callback;
    int result;
  };

public:
  static Persistent<FunctionTemplate> s_ct;

  static void Init(Handle<Object> target)
  {
    HandleScope scope;
    Local<FunctionTemplate> ft = FunctionTemplate::New(New);

    s_ct = Persistent<FunctionTemplate>::New(ft);
    s_ct->InstanceTemplate()->SetInternalFieldCount(1);
    s_ct->SetClassName(String::NewSymbol("Radius"));

    NODE_SET_PROTOTYPE_METHOD(s_ct, "InitRadius", InitRadius);
    NODE_SET_PROTOTYPE_METHOD(s_ct, "AvpairAddStr", AvpairAddStr);
    NODE_SET_PROTOTYPE_METHOD(s_ct, "AvpairAddInt", AvpairAddInt);
    NODE_SET_PROTOTYPE_METHOD(s_ct, "Auth", Auth);
    NODE_SET_PROTOTYPE_METHOD(s_ct, "Acct", Acct);
    NODE_SET_PROTOTYPE_METHOD(s_ct, "Busy", Busy);

    target->Set(String::NewSymbol("Radius"), s_ct->GetFunction());
  }

  static Handle<Value> New(const Arguments& args)
  {
    HandleScope scope;
    Radius * r = new Radius();
    r->Wrap(args.This());

    return args.This();
  }

  static Handle<Value> InitRadius(const Arguments& args)
  {
    HandleScope scope;
    Radius * r = ObjectWrap::Unwrap<Radius>(args.This());

    String::Utf8Value cfg(args[0]);
    
    r->send = NULL;
    r->busy = 0;

    // set up underlying library
    if ((r->rh = rc_read_config((char *)*cfg))) {
      if (rc_read_dictionary(r->rh, rc_conf_str(r->rh, (char *)"dictionary")) == 0) {
        return scope.Close(Integer::New(0));
      }
    }

    return scope.Close(Integer::New(1));
  }

  static Handle<Value> Busy(const Arguments& args)
  {
    HandleScope scope;
    Radius * r = ObjectWrap::Unwrap<Radius>(args.This());

    return scope.Close(Integer::New(r->busy));
  }


  static Handle<Value> AvpairAddStr(const Arguments& args)
  {
    HandleScope scope;
    Radius * r = ObjectWrap::Unwrap<Radius>(args.This());

    uint32_t type = args[0]->Uint32Value();
    String::Utf8Value str(args[1]);

    if (rc_avpair_add(r->rh, &(r->send), type, *str, -1, 0)) {
      return scope.Close(Integer::New(0));
    }

    return scope.Close(Integer::New(1));
  }

  static Handle<Value> AvpairAddInt(const Arguments& args)
  {
    HandleScope scope;
    Radius * r = ObjectWrap::Unwrap<Radius>(args.This());

    uint32_t type = args[0]->Uint32Value();
    uint32_t val(args[1]->Uint32Value());

    if (rc_avpair_add(r->rh, &(r->send), type, &val, -1, 0)) {
      return scope.Close(Integer::New(0));
    }

    return scope.Close(Integer::New(1));
  }

  /*
   * Auth Routines
   *
   */
  static Handle<Value> Auth(const Arguments& args)
  {
    HandleScope scope;
    Radius * r = ObjectWrap::Unwrap<Radius>(args.This());
    struct RadiusRequest * rad_req;

    REQ_FUN_ARG(0, cb);
    
    rad_req = (Radius::RadiusRequest*)malloc(sizeof(struct RadiusRequest));
    rad_req->r = r;
    rad_req->callback = Persistent<Function>::New(cb);

    r->busy = 1;
    eio_custom(EIO_Auth, EIO_PRI_DEFAULT, EIO_AfterAuth, rad_req);
    
    return scope.Close(Integer::New(0));
  }

  static int EIO_Auth(eio_req * req) {
    struct RadiusRequest * rad_req = (Radius::RadiusRequest*)req->data;
    Radius * r = rad_req->r;

    rad_req->result = rc_auth(r->rh, 0, r->send, &(r->received), r->msg);
 
    return 0;
  }

  static int EIO_AfterAuth(eio_req * req) {
    struct RadiusRequest * rad_req = (Radius::RadiusRequest*)req->data;
    Radius * r = rad_req->r;
    Local<Value> argv[1];

    TryCatch try_catch;

    argv[0] = Integer::New(rad_req->result);
    rad_req->callback->Call(Context::GetCurrent()->Global(), 1, argv);

    if (try_catch.HasCaught()) {
      FatalException(try_catch);
    }

    //    if (r->received) {
      //      printf("%s", rc_avpair_log(r->rh, r->received));
    // }

    if (r->send != NULL) {
      rc_avpair_free(r->send);
      r->send = NULL;
    }
    if (r->received != NULL) {
      rc_avpair_free(r->received);
      r->received = NULL;
    }
    free(rad_req);

    r->busy = 0;

    return 0;
  }

  /*
   * Acct Routines
   * 
   */
  static Handle<Value> Acct(const Arguments& args)
  {
    HandleScope scope;
    Radius * r = ObjectWrap::Unwrap<Radius>(args.This());
    struct RadiusRequest * rad_req;

    rad_req = (Radius::RadiusRequest*)malloc(sizeof(struct RadiusRequest));
    rad_req->r = r;

    r->busy = 1;
    eio_custom(EIO_Acct, EIO_PRI_DEFAULT, EIO_AfterAcct, rad_req);
    
    return scope.Close(Integer::New(0));
  }

  static int EIO_Acct(eio_req * req) {
    struct RadiusRequest * rad_req = (Radius::RadiusRequest*)req->data;
    Radius * r = rad_req->r;

    rad_req->result = rc_acct(r->rh, 0, r->send);
 
    return 0;
  }

  static int EIO_AfterAcct(eio_req * req) {
    struct RadiusRequest * rad_req = (Radius::RadiusRequest*)req->data;
    Radius * r = rad_req->r;
    Local<Value> argv[1];

    TryCatch try_catch;

    argv[0] = Integer::New(rad_req->result);
    rad_req->callback->Call(Context::GetCurrent()->Global(), 1, argv);

    if (try_catch.HasCaught()) {
      FatalException(try_catch);
    }

    if (r->send != NULL) {
      rc_avpair_free(r->send);
    }
    free(rad_req);

    r->busy = 0;

    return 0;
  }

};

Persistent<FunctionTemplate> Radius::s_ct;

extern "C" void
init(Handle<Object> target) {
  Radius::Init(target);
}