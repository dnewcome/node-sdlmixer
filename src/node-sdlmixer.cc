#include "node-sdlmixer.h"
#include "node_async_shim.h"

using namespace node_sdlmixer;

static int numChannels = 0;

static deque<int> availableChannels;

/**
 * Call this to claim an audio channel
 * Returns either an available channel (>=0) or
 * -1 (no available audio channel)
 */
static int claimAudioChannel() {
  // TODO: lock access to availableChannels?
  int result = -1;
  if (availableChannels.size() > 1) {
    result = availableChannels.front();
    availableChannels.pop_front();
  }
  return result;
}

/**
 * Call this to release a previously claimed audio channel
 * @param channel The channel number to release
 */
static void releaseAudioChannel(int channel) {
  // TODO: lock access to availableChannels?
  availableChannels.push_back(channel);
}

async_rtn doing_play (uv_work_t *req) {
  struct playInfo * pi = (struct playInfo *) req->data;

  /* Load the requested wave file */
  pi->wave = Mix_LoadWAV(pi->name);

  //printf("Playing [%s] on channel[%d]\n", pi->name, pi->channel);
  /* Play and then exit */
  Mix_PlayChannel(pi->channel, pi->wave, 0);
  RETURN_ASYNC
}

async_rtn after_doing_play (uv_work_t *req) {
  RETURN_ASYNC_AFTER
}

Handle<Value> SDLMixer::Play(const Arguments& args) {
  HandleScope scope;

  SDLMixer* sm = ObjectWrap::Unwrap<SDLMixer>(args.This());

  const char *usage = "usage: play(fileName, <callbackFunc>)";
  const char *noMoreChannels = "Out of available channels";
  // TODO: optional 2nd callbackFunc parameter?
  if (args.Length() < 1) {
    return ThrowException(Exception::Error(String::New(usage)));
  }
  int channel = claimAudioChannel();

  if (channel < 0) {
    return ThrowException(Exception::Error(String::New(noMoreChannels)));
  }

  String::Utf8Value fileName(args[0]);
  Local<Function> cb = Local<Function>::Cast(args[1]);

  playInfo *pi = (playInfo *) malloc(sizeof(struct playInfo)
      + fileName.length() + 1);

  pi->cb = Persistent<Function>::New(cb);
  pi->doCallback = args[1]->IsFunction();
  pi->channel = channel;
  pi->wave = NULL;
  strncpy(pi->name, *fileName, fileName.length() + 1);

  playInfoChannelList[channel] = pi;

  if (playDoneEvent == NULL) {
    //printf("Construct playDoneEvent\n");
    playDoneEvent = new AsyncPlayDone(sm, PlayDoneCallback);
  }

  BEGIN_ASYNC(pi, doing_play, after_doing_play);

  return scope.Close(args[0]);
}

Persistent<FunctionTemplate> SDLMixer::constructor_template;
vector<playInfo *> SDLMixer::playInfoChannelList;
SDLMixer::AsyncPlayDone *SDLMixer::playDoneEvent = NULL;

SDLMixer::SDLMixer() {
}

SDLMixer::~SDLMixer() {
}

void SDLMixer::Initialize(Handle<Object> target) {
  HandleScope scope;

  Local<FunctionTemplate> t = FunctionTemplate::New(New);
  constructor_template = Persistent<FunctionTemplate>::New(t);
  constructor_template->InstanceTemplate()->SetInternalFieldCount(1);
  constructor_template->SetClassName(String::NewSymbol("SDLMixer"));

  NODE_SET_PROTOTYPE_METHOD(constructor_template, "play", Play);
  target->Set(String::NewSymbol("SDLMixer"),
      constructor_template->GetFunction());
}

Handle<Value> SDLMixer::New(const Arguments &args) {
  HandleScope scope;

  if (SDL_Init(SDL_INIT_AUDIO) < 0) {
    return ThrowException(Exception::TypeError(String::New(SDL_GetError())));
  }

  int audio_rate;
  Uint16 audio_format;
  int audio_channels;

  /* Initialize variables */
  audio_rate = MIX_DEFAULT_FREQUENCY;
  audio_format = MIX_DEFAULT_FORMAT;
  audio_channels = 2;

  /* Open the audio device */
  if (Mix_OpenAudio(audio_rate, audio_format, audio_channels, 4096) < 0) {
    SDL_Quit();
    return ThrowException(Exception::TypeError(String::New(SDL_GetError())));
  }

  Mix_ChannelFinished(SDLMixer::ChannelFinished);

  numChannels = Mix_AllocateChannels(32);

  playInfoChannelList.resize(numChannels);

  for (int x = 0; x < numChannels; x++) {
    availableChannels.push_back(x);
    playInfoChannelList[x] = NULL;
  }

  Mix_QuerySpec(&audio_rate, &audio_format, &audio_channels);

  args.This()->Set(String::NewSymbol("audioRate"), Integer::New(audio_rate),
      ReadOnly);
  args.This()->Set(String::NewSymbol("audioFormat"), Integer::New((audio_format
      & 0xFF)), ReadOnly);
  args.This()->Set(String::NewSymbol("audioChannels"), String::New(
      (audio_channels > 2) ? "surround" : (audio_channels > 1) ? "stereo"
          : "mono"), ReadOnly);
  args.This()->Set(String::NewSymbol("numberOfAudioChannels"), Integer::New(
      numChannels));

  SDLMixer *sm = new SDLMixer();

  sm->Wrap(args.This());
  return args.This();
}

void SDLMixer::ChannelFinished(int channel) {
  //printf("SDLMixer::ChannelFinished(%d)\n", channel);

  playInfo *item = playInfoChannelList[channel];
  //printf("item %d, playDoneEvent %d\n", item, playDoneEvent);
  if ((item != NULL) && (playDoneEvent != NULL)) {
    playDoneEvent->send(item);
  }
}

void SDLMixer::PlayDoneCallback(SDLMixer *sm, playInfo *pi) {
  HandleScope scope;
  //printf("SDLMixer::PlayDoneCallback(%d)\n", pi->channel);

  playInfoChannelList[pi->channel] = NULL;
  releaseAudioChannel(pi->channel);

  //printf("availableChannels[%d]\n", (unsigned int)availableChannels.size());

  if (availableChannels.size() == (unsigned int)numChannels) {
    //printf("Delete playDoneEvent\n");
    delete playDoneEvent;
    playDoneEvent = NULL;
  }

  Local<Value> argv[2];
  argv[0] = Local<Value>::New(String::New(pi->name));
  argv[1] = Local<Value>::New(Integer::New(pi->channel));

  if (pi->doCallback) {
    TryCatch try_catch;
    pi->cb->Call(Context::GetCurrent()->Global(), 2, argv);
    if (try_catch.HasCaught()) {
      FatalException(try_catch);
    }
  }

  Mix_FreeChunk(pi->wave);
  pi->wave = NULL;

  pi->cb.Dispose();
  free(pi);
}

extern "C" void init(Handle<Object> target) {
  HandleScope scope;

  SDLMixer::Initialize(target);
}
