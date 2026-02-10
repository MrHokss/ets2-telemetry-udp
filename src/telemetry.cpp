/**
 * ETS2/ATS Telemetry Plugin (Linux + Windows)
 *
 * - Based on SCS SDK telemetry example
 * - Outputs JSON over UDP to 127.0.0.1:49001
 *
 * Fields:
 *  speed (m/s), rpm, gear, dgear, steer, throttle, brake, clutch, cruise
 */

#ifdef _WIN32
#  define WINVER 0x0501
#  define _WIN32_WINNT 0x0501
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  pragma comment(lib, "ws2_32.lib")
#endif


#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stdarg.h>
#include <string.h>

#ifdef __linux__
#  include <arpa/inet.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

// SDK
#include "scssdk_telemetry.h"
#include "eurotrucks2/scssdk_eut2.h"
#include "eurotrucks2/scssdk_telemetry_eut2.h"
#include "amtrucks/scssdk_ats.h"
#include "amtrucks/scssdk_telemetry_ats.h"

#define UNUSED(x)

/**
 * @brief Logging support.
 */
FILE *log_file = NULL;

/**
 * @brief Tracking of paused state of the game.
 */
bool output_paused = true;

/**
 * @brief Should we print the data header next time
 * we are printing the data?
 */
bool print_header = true;

/**
 * @brief Last timestamp we received.
 */
scs_timestamp_t last_timestamp = static_cast<scs_timestamp_t>(-1);

/**
 * @brief Combined telemetry data.
 */
struct telemetry_state_t
{
	scs_timestamp_t timestamp;
	scs_timestamp_t raw_rendering_timestamp;
	scs_timestamp_t raw_simulation_timestamp;
	scs_timestamp_t raw_paused_simulation_timestamp;

	bool  orientation_available;
	float heading;
	float pitch;
	float roll;

	float speed;
	float rpm;
	int   gear;

	// Inputs / controls
	float input_steering;
	float input_throttle;
	float input_brake;
	float input_clutch;

	// Cruise control state/value (game exposes it as float)
	float cruise_control;

	// What the UI shows (can differ from engine_gear for some setups)
	int   displayed_gear;

} telemetry;

/**
 * @brief Function writting message to the game internal log.
 */
scs_log_t game_log = NULL;

// ===== UDP output (localhost) =====
#if defined(__linux__) || defined(_WIN32)

#ifdef _WIN32
static SOCKET  g_udp_fd = INVALID_SOCKET;
static WSADATA g_wsa;
#else
static int     g_udp_fd = -1;
#endif

static sockaddr_in g_udp_addr{};
static bool g_udp_ready = false;

// Cache last values we care about
static float g_udp_speed = 0.0f;
static float g_udp_rpm   = 0.0f;

static void udp_open()
{
	#ifdef _WIN32
	if (WSAStartup(MAKEWORD(2, 2), &g_wsa) != 0) {
		g_udp_ready = false;
		return;
	}
	g_udp_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (g_udp_fd == INVALID_SOCKET) {
		g_udp_ready = false;
		WSACleanup();
		return;
	}
	#else
	g_udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (g_udp_fd < 0) {
		g_udp_ready = false;
		return;
	}
	#endif

	memset(&g_udp_addr, 0, sizeof(g_udp_addr));
	g_udp_addr.sin_family = AF_INET;
	g_udp_addr.sin_port = htons(49001);

	// 127.0.0.1 only (safe default)
	#ifdef _WIN32
	g_udp_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

	#else
	g_udp_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	#endif

	g_udp_ready = true;
}

static void udp_close()
{
	#ifdef _WIN32
	if (g_udp_fd != INVALID_SOCKET) {
		closesocket(g_udp_fd);
	}
	g_udp_fd = INVALID_SOCKET;
	WSACleanup();
	#else
	if (g_udp_fd >= 0) {
		close(g_udp_fd);
	}
	g_udp_fd = -1;
	#endif
	g_udp_ready = false;
}

static void udp_send_json()
{
	if (!g_udp_ready) return;

	// Make sure "-0.000" doesn't show
	float speed = g_udp_speed;
	if (speed == 0.0f) speed = 0.0f;

	char buf[512];
	const int n = snprintf(
		buf,
		sizeof(buf),
						   "{\"speed\":%.3f,\"rpm\":%.1f,\"gear\":%d,\"dgear\":%d,"
						   "\"steer\":%.3f,\"throttle\":%.3f,\"brake\":%.3f,\"clutch\":%.3f,"
						   "\"cruise\":%.3f}\n",
						speed,
						g_udp_rpm,
						telemetry.gear,
						telemetry.displayed_gear,
						telemetry.input_steering,
						telemetry.input_throttle,
						telemetry.input_brake,
						telemetry.input_clutch,
						telemetry.cruise_control
	);

	if (n > 0) {
		sendto(
			g_udp_fd,
		 buf,
		 (int)n,
			   0,
		 (sockaddr*)&g_udp_addr,
			   (int)sizeof(g_udp_addr)
		);
	}
}
#endif

// Management of the log file.

bool init_log(void)
{
	if (log_file) {
		return true;
	}
	log_file = fopen("telemetry.log", "wt");
	if (!log_file) {
		return false;
	}
	fprintf(log_file, "Log opened\n");
	return true;
}

void finish_log(void)
{
	if (!log_file) {
		return;
	}
	fprintf(log_file, "Log ended\n");
	fclose(log_file);
	log_file = NULL;
}

void log_print(const char *const text, ...)
{
	if (!log_file) {
		return;
	}
	va_list args;
	va_start(args, text);
	vfprintf(log_file, text, args);
	va_end(args);
}

void log_line(const char *const text, ...)
{
	if (!log_file) {
		return;
	}
	va_list args;
	va_start(args, text);
	vfprintf(log_file, text, args);
	fprintf(log_file, "\n");
	va_end(args);
}

// Handling of individual events.

SCSAPI_VOID telemetry_frame_start(const scs_event_t UNUSED(event), const void *const event_info, const scs_context_t UNUSED(context))
{
	const struct scs_telemetry_frame_start_t *const info = static_cast<const scs_telemetry_frame_start_t *>(event_info);

	if (last_timestamp == static_cast<scs_timestamp_t>(-1)) {
		last_timestamp = info->paused_simulation_time;
	}

	if (info->flags & SCS_TELEMETRY_FRAME_START_FLAG_timer_restart) {
		last_timestamp = 0;
	}

	telemetry.timestamp += (info->paused_simulation_time - last_timestamp);
	last_timestamp = info->paused_simulation_time;

	telemetry.raw_rendering_timestamp = info->render_time;
	telemetry.raw_simulation_timestamp = info->simulation_time;
	telemetry.raw_paused_simulation_timestamp = info->paused_simulation_time;
}

SCSAPI_VOID telemetry_frame_end(const scs_event_t UNUSED(event), const void *const UNUSED(event_info), const scs_context_t UNUSED(context))
{
	if (output_paused) {
		return;
	}

	if (print_header) {
		print_header = false;
		log_line("timestamp[us];raw rendering timestamp[us];raw simulation timestamp[us];raw paused simulation timestamp[us];heading[deg];pitch[deg];roll[deg];speed[m/s];rpm;gear");
	}

	log_print("%" SCS_PF_U64 ";%" SCS_PF_U64 ";%" SCS_PF_U64 ";%" SCS_PF_U64,
			  telemetry.timestamp,
		   telemetry.raw_rendering_timestamp,
		   telemetry.raw_simulation_timestamp,
		   telemetry.raw_paused_simulation_timestamp);

	if (telemetry.orientation_available) {
		log_print(";%f;%f;%f", telemetry.heading, telemetry.pitch, telemetry.roll);
	} else {
		log_print(";---;---;---");
	}

	log_line(";%f;%f;%d", telemetry.speed, telemetry.rpm, telemetry.gear);
}

SCSAPI_VOID telemetry_pause(const scs_event_t event, const void *const UNUSED(event_info), const scs_context_t UNUSED(context))
{
	output_paused = (event == SCS_TELEMETRY_EVENT_paused);
	log_line(output_paused ? "Telemetry paused" : "Telemetry unpaused");
	print_header = true;
}

void telemetry_print_attributes(const scs_named_value_t *const attributes)
{
	for (const scs_named_value_t *current = attributes; current->name; ++current) {
		log_print("  %s", current->name);
		if (current->index != SCS_U32_NIL) {
			log_print("[%u]", static_cast<unsigned>(current->index));
		}
		log_print(" : ");

		switch (current->value.type) {
			case SCS_VALUE_TYPE_INVALID:  log_line("none"); break;
			case SCS_VALUE_TYPE_bool:     log_line("bool = %s", current->value.value_bool.value ? "true" : "false"); break;
			case SCS_VALUE_TYPE_s32:      log_line("s32 = %d", static_cast<int>(current->value.value_s32.value)); break;
			case SCS_VALUE_TYPE_u32:      log_line("u32 = %u", static_cast<unsigned>(current->value.value_u32.value)); break;
			case SCS_VALUE_TYPE_s64:      log_line("s64 = %" SCS_PF_S64, current->value.value_s64.value); break;
			case SCS_VALUE_TYPE_u64:      log_line("u64 = %" SCS_PF_U64, current->value.value_u64.value); break;
			case SCS_VALUE_TYPE_float:    log_line("float = %f", current->value.value_float.value); break;
			case SCS_VALUE_TYPE_double:   log_line("double = %f", current->value.value_double.value); break;

			case SCS_VALUE_TYPE_fvector:
				log_line("fvector = (%f,%f,%f)",
						 current->value.value_fvector.x,
			 current->value.value_fvector.y,
			 current->value.value_fvector.z);
				break;

			case SCS_VALUE_TYPE_dvector:
				log_line("dvector = (%f,%f,%f)",
						 current->value.value_dvector.x,
			 current->value.value_dvector.y,
			 current->value.value_dvector.z);
				break;

			case SCS_VALUE_TYPE_euler:
				log_line("euler = h:%f p:%f r:%f",
						 current->value.value_euler.heading * 360.0f,
			 current->value.value_euler.pitch * 360.0f,
			 current->value.value_euler.roll * 360.0f);
				break;

			case SCS_VALUE_TYPE_fplacement:
				log_line("fplacement = (%f,%f,%f) h:%f p:%f r:%f",
						 current->value.value_fplacement.position.x,
			 current->value.value_fplacement.position.y,
			 current->value.value_fplacement.position.z,
			 current->value.value_fplacement.orientation.heading * 360.0f,
			 current->value.value_fplacement.orientation.pitch * 360.0f,
			 current->value.value_fplacement.orientation.roll * 360.0f);
				break;

			case SCS_VALUE_TYPE_dplacement:
				log_line("dplacement = (%f,%f,%f) h:%f p:%f r:%f",
						 current->value.value_dplacement.position.x,
			 current->value.value_dplacement.position.y,
			 current->value.value_dplacement.position.z,
			 current->value.value_dplacement.orientation.heading * 360.0f,
			 current->value.value_dplacement.orientation.pitch * 360.0f,
			 current->value.value_dplacement.orientation.roll * 360.0f);
				break;

			case SCS_VALUE_TYPE_string:
				log_line("string = %s", current->value.value_string.value);
				break;

			default:
				log_line("unknown");
				break;
		}
	}
}

SCSAPI_VOID telemetry_configuration(const scs_event_t event, const void *const event_info, const scs_context_t UNUSED(context))
{
	const struct scs_telemetry_configuration_t *const info = static_cast<const scs_telemetry_configuration_t *>(event_info);
	log_line("Configuration: %s", info->id);
	telemetry_print_attributes(info->attributes);
	print_header = true;
}

SCSAPI_VOID telemetry_gameplay_event(const scs_event_t event, const void *const event_info, const scs_context_t UNUSED(context))
{
	const struct scs_telemetry_gameplay_event_t *const info = static_cast<const scs_telemetry_gameplay_event_t *>(event_info);
	log_line("Gameplay event: %s", info->id);
	telemetry_print_attributes(info->attributes);
	print_header = true;
}

// Handling of individual channels.

SCSAPI_VOID telemetry_store_orientation(const scs_string_t UNUSED(name), const scs_u32_t UNUSED(index), const scs_value_t *const value, const scs_context_t context)
{
	assert(context);
	telemetry_state_t *const state = static_cast<telemetry_state_t *>(context);

	if (!value) {
		state->orientation_available = false;
		return;
	}

	assert(value->type == SCS_VALUE_TYPE_euler);
	state->orientation_available = true;
	state->heading = value->value_euler.heading * 360.0f;
	state->pitch   = value->value_euler.pitch   * 360.0f;
	state->roll    = value->value_euler.roll    * 360.0f;
}

SCSAPI_VOID telemetry_store_float(const scs_string_t UNUSED(name), const scs_u32_t UNUSED(index), const scs_value_t *const value, const scs_context_t context)
{
	assert(value);
	assert(value->type == SCS_VALUE_TYPE_float);
	assert(context);

	*static_cast<float *>(context) = value->value_float.value;

	#if defined(__linux__) || defined(_WIN32)
	if (context == &telemetry.speed) {
		g_udp_speed = value->value_float.value;
		udp_send_json();
	} else if (context == &telemetry.rpm) {
		g_udp_rpm = value->value_float.value;
		udp_send_json();
	} else if (context == &telemetry.input_steering ||
		context == &telemetry.input_throttle ||
		context == &telemetry.input_brake ||
		context == &telemetry.input_clutch ||
		context == &telemetry.cruise_control) {
		udp_send_json();
		}
		#endif
}

SCSAPI_VOID telemetry_store_s32(const scs_string_t UNUSED(name), const scs_u32_t UNUSED(index), const scs_value_t *const value, const scs_context_t context)
{
	assert(value);
	assert(value->type == SCS_VALUE_TYPE_s32);
	assert(context);

	*static_cast<int *>(context) = value->value_s32.value;

	#if defined(__linux__) || defined(_WIN32)
	if (context == &telemetry.gear || context == &telemetry.displayed_gear) {
		udp_send_json();
	}
	#endif
}

/**
 * @brief Telemetry API initialization function.
 *
 * See scssdk_telemetry.h
 */
SCSAPI_RESULT scs_telemetry_init(const scs_u32_t version, const scs_telemetry_init_params_t *const params)
{
	if (version != SCS_TELEMETRY_VERSION_1_01) {
		return SCS_RESULT_unsupported;
	}

	const scs_telemetry_init_params_v101_t *const version_params =
	static_cast<const scs_telemetry_init_params_v101_t *>(params);

	if (!init_log()) {
		version_params->common.log(SCS_LOG_TYPE_error, "Unable to initialize the log file");
		return SCS_RESULT_generic_error;
	}

	#if defined(__linux__) || defined(_WIN32)
	udp_open();
	#endif

	log_line("Game '%s' %u.%u",
			 version_params->common.game_id,
		  SCS_GET_MAJOR_VERSION(version_params->common.game_version),
			 SCS_GET_MINOR_VERSION(version_params->common.game_version));

	if (strcmp(version_params->common.game_id, SCS_GAME_ID_EUT2) == 0) {

		const scs_u32_t MINIMAL_VERSION = SCS_TELEMETRY_EUT2_GAME_VERSION_1_00;
		if (version_params->common.game_version < MINIMAL_VERSION) {
			log_line("WARNING: Too old version of the game, some features might behave incorrectly");
		}

		const scs_u32_t IMPLEMENTED_VERSION = SCS_TELEMETRY_EUT2_GAME_VERSION_CURRENT;
		if (SCS_GET_MAJOR_VERSION(version_params->common.game_version) >
			SCS_GET_MAJOR_VERSION(IMPLEMENTED_VERSION)) {
			log_line("WARNING: Too new major version of the game, some features might behave incorrectly");
			}
	}
	else if (strcmp(version_params->common.game_id, SCS_GAME_ID_ATS) == 0) {

		const scs_u32_t MINIMAL_VERSION = SCS_TELEMETRY_ATS_GAME_VERSION_1_00;
		if (version_params->common.game_version < MINIMAL_VERSION) {
			log_line("WARNING: Too old version of the game, some features might behave incorrectly");
		}

		const scs_u32_t IMPLEMENTED_VERSION = SCS_TELEMETRY_ATS_GAME_VERSION_CURRENT;
		if (SCS_GET_MAJOR_VERSION(version_params->common.game_version) >
			SCS_GET_MAJOR_VERSION(IMPLEMENTED_VERSION)) {
			log_line("WARNING: Too new major version of the game, some features might behave incorrectly");
			}
	}
	else {
		log_line("WARNING: Unsupported game, some features or values might behave incorrectly");
	}

	const bool events_registered =
	(version_params->register_for_event(SCS_TELEMETRY_EVENT_frame_start, telemetry_frame_start, NULL) == SCS_RESULT_ok) &&
	(version_params->register_for_event(SCS_TELEMETRY_EVENT_frame_end, telemetry_frame_end, NULL) == SCS_RESULT_ok) &&
	(version_params->register_for_event(SCS_TELEMETRY_EVENT_paused, telemetry_pause, NULL) == SCS_RESULT_ok) &&
	(version_params->register_for_event(SCS_TELEMETRY_EVENT_started, telemetry_pause, NULL) == SCS_RESULT_ok);

	if (!events_registered) {
		version_params->common.log(SCS_LOG_TYPE_error, "Unable to register event callbacks");
		return SCS_RESULT_generic_error;
	}

	version_params->register_for_event(SCS_TELEMETRY_EVENT_configuration, telemetry_configuration, NULL);
	version_params->register_for_event(SCS_TELEMETRY_EVENT_gameplay, telemetry_gameplay_event, NULL);

	// Channels we care about (minimal + useful)
	version_params->register_for_channel(
		SCS_TELEMETRY_TRUCK_CHANNEL_world_placement,
		SCS_U32_NIL,
		SCS_VALUE_TYPE_euler,
		SCS_TELEMETRY_CHANNEL_FLAG_no_value,
		telemetry_store_orientation,
		&telemetry
	);

	version_params->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_speed,       SCS_U32_NIL, SCS_VALUE_TYPE_float, SCS_TELEMETRY_CHANNEL_FLAG_none, telemetry_store_float, &telemetry.speed);
	version_params->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_engine_rpm,  SCS_U32_NIL, SCS_VALUE_TYPE_float, SCS_TELEMETRY_CHANNEL_FLAG_none, telemetry_store_float, &telemetry.rpm);
	version_params->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_engine_gear, SCS_U32_NIL, SCS_VALUE_TYPE_s32,   SCS_TELEMETRY_CHANNEL_FLAG_none, telemetry_store_s32,   &telemetry.gear);

	version_params->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_displayed_gear, SCS_U32_NIL, SCS_VALUE_TYPE_s32, SCS_TELEMETRY_CHANNEL_FLAG_none, telemetry_store_s32, &telemetry.displayed_gear);

	version_params->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_input_steering, SCS_U32_NIL, SCS_VALUE_TYPE_float, SCS_TELEMETRY_CHANNEL_FLAG_none, telemetry_store_float, &telemetry.input_steering);
	version_params->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_input_throttle, SCS_U32_NIL, SCS_VALUE_TYPE_float, SCS_TELEMETRY_CHANNEL_FLAG_none, telemetry_store_float, &telemetry.input_throttle);
	version_params->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_input_brake,    SCS_U32_NIL, SCS_VALUE_TYPE_float, SCS_TELEMETRY_CHANNEL_FLAG_none, telemetry_store_float, &telemetry.input_brake);
	version_params->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_input_clutch,   SCS_U32_NIL, SCS_VALUE_TYPE_float, SCS_TELEMETRY_CHANNEL_FLAG_none, telemetry_store_float, &telemetry.input_clutch);

	version_params->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_cruise_control, SCS_U32_NIL, SCS_VALUE_TYPE_float, SCS_TELEMETRY_CHANNEL_FLAG_none, telemetry_store_float, &telemetry.cruise_control);

	game_log = version_params->common.log;
	game_log(SCS_LOG_TYPE_message, "Initializing ETS2 telemetry UDP JSON plugin");

	memset(&telemetry, 0, sizeof(telemetry));
	print_header = true;
	last_timestamp = static_cast<scs_timestamp_t>(-1);

	output_paused = true;
	return SCS_RESULT_ok;
}

/**
 * @brief Telemetry API deinitialization function.
 *
 * See scssdk_telemetry.h
 */
SCSAPI_VOID scs_telemetry_shutdown(void)
{
	game_log = NULL;

	#if defined(__linux__) || defined(_WIN32)
	udp_close();
	#endif

	finish_log();
}

// Cleanup

#ifdef _WIN32
BOOL APIENTRY DllMain(HMODULE module, DWORD reason_for_call, LPVOID reserved)
{
	UNUSED(module);
	UNUSED(reserved);

	if (reason_for_call == DLL_PROCESS_DETACH) {
		finish_log();
	}
	return TRUE;
}
#endif

#ifdef __linux__
void __attribute__ ((destructor)) unload(void)
{
	finish_log();
}
#endif
