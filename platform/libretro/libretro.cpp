#include <stdio.h>
#include <string.h>
#include <array>

#include "libretro.h"

#include "../../source/vm.h"
#include "../../source/PicoRam.h"
#include "../../source/Audio.h"
#include "../../source/host.h"
#include "../../source/hostVmShared.h"
#include "../../source/nibblehelpers.h"
#include "../../source/filehelpers.h"
#include "libretrohosthelpers.h"


//since this is a C api, we need to mark all these as extern
//so the compiler doesn't mangle the symbol names
#define EXPORT extern "C" RETRO_API

static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_environment_t enviro_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_log_printf_t log_cb;



#define SAMPLERATE 22050
#define SAMPLESPERFRAME (SAMPLERATE / 30)
#define NUM_BUFFERS 2
const size_t audioBufferSize = SAMPLESPERFRAME * NUM_BUFFERS;

int16_t audioBuffer[audioBufferSize];

const int PicoScreenWidth = 128;
const int PicoScreenHeight = 128;

const int BytesPerPixel = 2;

const size_t screenBufferSize = PicoScreenWidth*PicoScreenHeight;

uint16_t screenBuffer[screenBufferSize];

uint16_t _rgb565Colors[144];

Vm* _vm;
PicoRam* _memory;
Audio* _audio;
Host* _host;

double prev_frame_time = 0;
double frame_time = 0;

static void frame_time_cb(retro_usec_t usec)
{
    prev_frame_time = frame_time;
    frame_time = usec / 1000000.0;
}


EXPORT void retro_set_environment(retro_environment_t cb)
{
    //allow to run built in bios if no rom is passed
    enviro_cb = cb;
    bool no_rom = true;
    enviro_cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_rom);

    //this came from another stub... not sure if we need it?
    char const *system_dir;
    enviro_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir);

    struct retro_frame_time_callback frame_cb = { frame_time_cb, 1000000 / 60 };
    enviro_cb(RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK, &frame_cb);
}

EXPORT void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
EXPORT void retro_set_audio_sample(retro_audio_sample_t cb) { audio_cb = cb; }
EXPORT void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
EXPORT void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
EXPORT void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

EXPORT void retro_init()
{
    retro_log_callback log;
	if (enviro_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
	{
		log_cb = log.log;
        log_cb(RETRO_LOG_INFO, "Retro init  called. setting up fake-08 host\n");
	}
	else
	{
		log_cb = nullptr;
        printf("retro init called. no retro logger\n");
	}

    //called once. do setup (create host and vm?)
    _host = new Host();

    char const *save_dir;
    if (enviro_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save_dir) && save_dir) {
        string saveDirStr = save_dir;
        if (! hasEnding(saveDirStr, "/")) {
            saveDirStr = saveDirStr + "/";
        }
        _host->overrideLogFilePrefix(saveDirStr.c_str());
    }

	_memory = new PicoRam();
	_audio = new Audio(_memory);

    _vm = new Vm(_host, _memory, nullptr, nullptr, _audio);

    _host->setUpPaletteColors();
    Color* paletteColors = _host->GetPaletteColors();

	_host->oneTimeSetup(_audio);

    for(int i = 0; i < 144; i++){
        _rgb565Colors[i] = (((paletteColors[i].Red & 0xf8)<<8) + ((paletteColors[i].Green & 0xfc)<<3)+(paletteColors[i].Blue>>3));
    }

    _vm->SetCartList(_host->listcarts());

    _vm->LoadBiosCart();
}

EXPORT void retro_deinit()
{
    if (log_cb) {
        log_cb(RETRO_LOG_INFO, "Retro deinit called. tearing down\n");
    }
    //delete things created in init
    _vm->CloseCart();
    _host->oneTimeCleanup();
    delete _vm;
    delete _host;
}

EXPORT unsigned retro_api_version()
{
   return RETRO_API_VERSION;
}

EXPORT void retro_get_system_info(struct retro_system_info *info)
{
    memset(info, 0, sizeof(*info));
    info->library_name = "fake-08";
    info->library_version = "0.0.2.18"; //todo: get from build flags
    info->valid_extensions = "p8|png";
    info->need_fullpath = true; // we load our own carts for now
}

EXPORT void retro_get_system_av_info(struct retro_system_av_info *info)
{
    memset(info, 0, sizeof(*info));
    info->geometry.base_width = PicoScreenWidth;
    info->geometry.base_height = PicoScreenHeight;
    info->geometry.max_width = PicoScreenWidth;
    info->geometry.max_height = PicoScreenHeight;
    info->geometry.aspect_ratio = 1.f;
    info->timing.fps = 60.f; //todo: update this to 60, then handle 30 at callback level?
    info->timing.sample_rate = 22050.f;

    retro_pixel_format pf = RETRO_PIXEL_FORMAT_RGB565;
    enviro_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &pf);
}

EXPORT void retro_set_controller_port_device(unsigned port, unsigned device)
{
}

EXPORT void retro_reset()
{
    _vm->QueueCartChange(_vm->CurrentCartFilename());
}

static std::array<int, 7> buttons
{
    RETRO_DEVICE_ID_JOYPAD_LEFT,
    RETRO_DEVICE_ID_JOYPAD_RIGHT,
    RETRO_DEVICE_ID_JOYPAD_UP,
    RETRO_DEVICE_ID_JOYPAD_DOWN,
    RETRO_DEVICE_ID_JOYPAD_A,
    RETRO_DEVICE_ID_JOYPAD_B,
    RETRO_DEVICE_ID_JOYPAD_START,
};

uint8_t kHeld = 0;
uint8_t kDown = 0;

int16_t picoMouseX = 0;
int16_t picoMouseY = 0;
uint8_t mouseBtnState = 0;

size_t frame = 0;

uint8_t drawMode = 0;
int drawModeScaleX = 1;
int drawModeScaleY = 1;
int textureAngle = 0;
int flip = 0;

EXPORT void retro_run()
{
    //TODO: improve this so slower hardware can play 30fps games at full speed
    if (_vm->getTargetFps() == 60 || frame % 2 == 0)
    {
        input_poll_cb();

        uint8_t currKDown = 0;
        uint8_t currKHeld = 0;
        for (int i = 0; i < 7; i++) {
            bool down = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, buttons[i]);
            if (down) {
                currKHeld |= BITMASK(i);
                //if the key is a new push this frame, mark it as down as well
                if (!(kHeld & BITMASK(i))) {
                    currKDown |= BITMASK(i);
                }
            }
        }

        mouseBtnState = 0;

        if (_memory->drawState.devkitMode) {
            int16_t pressed = input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED);
            int16_t pointX = input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X);
            int16_t pointY = input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y);
            
            if (pressed) {
                picoMouseX = pointX * 64 / 32768 + 64;
                picoMouseY = pointY * 64 / 32768 + 64;
                mouseBtnState = 1;
            }
        }
        
        setInputState(currKDown, currKHeld, picoMouseX, picoMouseY, mouseBtnState);

        _vm->UpdateAndDraw();
        kHeld = currKHeld;
        kDown = currKDown;

        if (frame % 2 == 0) {
            _audio->FillAudioBuffer(&audioBuffer, 0, SAMPLESPERFRAME);
            audio_batch_cb(audioBuffer, SAMPLESPERFRAME);
        }

    }

    uint8_t* picoFb = _vm->GetPicoInteralFb();
    uint8_t* screenPaletteMap = _vm->GetScreenPaletteMap();

    drawMode = _memory->drawState.drawMode;

    drawModeScaleX = 1;
    drawModeScaleY = 1;
    switch(drawMode){
        case 1:
            drawModeScaleX = 2;
            textureAngle = 0;
            flip = 0;
            break;
        case 2:
            drawModeScaleY = 2;
            textureAngle = 0;
            flip = 0;
            break;
        case 3:
            drawModeScaleX = 2;
            drawModeScaleY = 2;
            textureAngle = 0;
            flip = 0;
            break;
        //todo: mirroring
        //case 4,6,7
        case 129:
            textureAngle = 0;
            flip = 1;
            break;
        case 130:
            textureAngle = 0;
            flip = 2;
            break;
        case 131:
            textureAngle = 0;
            flip = 3;
            break;
        case 133:
            textureAngle = 90;
            flip = 0;
            break;
        case 134:
            textureAngle = 180;
            flip = 0;
            break;
        case 135:
            textureAngle = 270;
            flip = 0;
            break;
        default:
            textureAngle = 0;
            flip = 0;
            break;
    }

    //TODO: handle rotation/flip/mirroring
    for(int scry = 0; scry < PicoScreenHeight; scry++) {
        for (int scrx = 0; scrx < PicoScreenWidth; scrx++) {
            int picox = scrx / drawModeScaleX;
            int picoy = scry / drawModeScaleY;
            screenBuffer[scry*128+scrx] = _rgb565Colors[screenPaletteMap[getPixelNibble(picox, picoy, picoFb)]];
        }
    }

    video_cb(&screenBuffer, PicoScreenWidth, PicoScreenHeight, PicoScreenWidth * BytesPerPixel);

    frame++;
}

//lua memory is 2 MB in size. 1 MB enough for non-globals?
//https://www.lexaloffle.com/dl/docs/pico-8_manual.html
//section 6.7: Memory
#define LUASTATEBUFFSIZE 1024*1024
char luaStateBuffer[LUASTATEBUFFSIZE];

EXPORT size_t retro_serialize_size()
{
    return sizeof(PicoRam) + sizeof(audioState_t) + LUASTATEBUFFSIZE;
}

EXPORT bool retro_serialize(void *data, size_t size)
{
    if (log_cb) {
        log_cb(RETRO_LOG_INFO, "retro_serialize. Checking size\n");
    }
    const size_t expectedSize = retro_serialize_size();
    if (size > expectedSize) {
        size = expectedSize;
    }
    else if (size < expectedSize) {
        return false;
    }

    if (log_cb) {
        log_cb(RETRO_LOG_INFO, "got size %d\n", size);
    }

    if (log_cb) {
        log_cb(RETRO_LOG_INFO, "setting up lua state buffer\n");
    }
    
    memset(luaStateBuffer, 0, LUASTATEBUFFSIZE);

    if (log_cb) {
        log_cb(RETRO_LOG_INFO, "serializing lua state\n");
    }
    size_t offset = 0;
    size_t luaStateSize = _vm->serializeLuaState(luaStateBuffer);

    if (log_cb) {
        log_cb(RETRO_LOG_INFO, "copying lua state size to buffer\n");
    }
    memcpy(((char*)data + offset), &luaStateSize, sizeof(size_t));
    offset += sizeof(size_t);

    if (log_cb) {
        log_cb(RETRO_LOG_INFO, "copying lua state to buffer\n");
    }

    memcpy(((char*)data + offset), luaStateBuffer, luaStateSize);
    offset += luaStateSize;

    if (log_cb) {
        log_cb(RETRO_LOG_INFO, "copying pico 8 memory to buffer\n");
    }

    memcpy(((char*)data + offset), _memory->data, sizeof(PicoRam));
    offset += sizeof(PicoRam);

    if (log_cb) {
        log_cb(RETRO_LOG_INFO, "copying audio state size to buffer\n");
    }

    memcpy(((char*)data + offset), _audio->getAudioState(), sizeof(audioState_t));
    offset += sizeof(audioState_t);

    if (log_cb) {
        log_cb(RETRO_LOG_INFO, "returning true\n");
    }

    return true;
}

EXPORT bool retro_unserialize(const void *data, size_t size)
{
    if (log_cb) {
        log_cb(RETRO_LOG_INFO, "lua deserialize\n");
    }

    if (log_cb) {
        log_cb(RETRO_LOG_INFO, "setting up lua state buffer\n");
    }
    memset(luaStateBuffer, 0, LUASTATEBUFFSIZE);

    if (log_cb) {
        log_cb(RETRO_LOG_INFO, "copying lua state from buffer to var\n");
    }
    size_t offset = 0;
    size_t luaStateSize;
    memcpy(&luaStateSize, ((char*)data + offset),  sizeof(size_t));
    offset += sizeof(size_t);

    if (log_cb) {
        log_cb(RETRO_LOG_INFO, "got lua state size %d\n", luaStateSize);
    }

    if (log_cb) {
        log_cb(RETRO_LOG_INFO, "copying lua state\n");
    }

    memcpy(luaStateBuffer, ((char*)data + offset), luaStateSize);
    offset += luaStateSize;

    if (log_cb) {
        log_cb(RETRO_LOG_INFO, "deserializing lua state\n");
    }
    _vm->deserializeLuaState(luaStateBuffer, luaStateSize);

    if (log_cb) {
        log_cb(RETRO_LOG_INFO, "copying pico 8 memory\n");
    }

    memcpy(_memory->data, ((char*)data + offset), sizeof(PicoRam));
    offset += sizeof(PicoRam);

    if (log_cb) {
        log_cb(RETRO_LOG_INFO, "copying audio state\n");
    }

    memcpy(_audio->getAudioState(), ((char*)data + offset), sizeof(audioState_t));
    offset += sizeof(audioState_t);
    
    return true;
}

EXPORT void retro_cheat_reset()
{
}

EXPORT void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
}

EXPORT bool retro_load_game(struct retro_game_info const *info)
{
    auto containingDir = getDirectory(info->path);

    if (containingDir.length() > 0) {
        setCartDirectory(containingDir);
    }

    _vm->QueueCartChange(info->path);
    return true;
}

EXPORT bool retro_load_game_special(unsigned game_type,
    const struct retro_game_info *info, size_t num_info)
{
    return false;
}

EXPORT void retro_unload_game()
{
    _vm->CloseCart();
}

EXPORT unsigned retro_get_region()
{
    return 0;
}

EXPORT void *retro_get_memory_data(unsigned id)
{
    //move cart data saving to here?
    return nullptr;
}

EXPORT size_t retro_get_memory_size(unsigned id)
{
    return 0;
}
