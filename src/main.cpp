/*
 * main.cpp
 *
 *  Created on: 2014. 6. 10.
 *      Author: oskwon
 */

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <poll.h>
#include <errno.h>
#include <signal.h>

#include <string>

#include <sys/types.h>
#include <sys/stat.h>

#include "Util.h"
#include "Logger.h"

#include "Http.h"
#include "Mpeg.h"

#include "Demuxer.h"
#include "Encoder.h"
#include "UriDecoder.h"
#include "SessionMap.h"

using namespace std;
//----------------------------------------------------------------------

#define BUFFFER_SIZE (188 * 256)

void show_help();
void signal_handler(int sig_no);

void *source_thread_main(void *params);
void *streaming_thread_main(void *params);

static Source *source = 0;
static Encoder *encoder = 0;

static bool is_terminated = true;
static int source_thread_id, stream_thread_id;
static pthread_t source_thread_handle, stream_thread_handle;

#define TSP_CHECKER_TEMPLETE "/tmp/tsp_status.%d"

pid_t tsp_pid = 0, checker_pid = 0;
unsigned long last_updated_time = 0;
//----------------------------------------------------------------------

void release_webif_record()
{
	if (source) {
		Source::source_type_t type = source->get_source_type();
		if (type == Source::SOURCE_TYPE_LIVE) {
			Demuxer *demux = (Demuxer*)source;
			demux->disconnect_webif_socket();
			DEBUG("---->> disconnected at %s <<----", get_timestamp());
		}
	}
}
//----------------------------------------------------------------------

bool terminated()
{
	return is_terminated;
}
//----------------------------------------------------------------------

inline void release_resource()
{
	DEBUG("release resource start");
	if (encoder) { 
		delete encoder;
		encoder = 0;
		DEBUG("encoder release ok");
	}
	if (source) {
		delete source;
		source = 0;
		DEBUG("source release ok");
	}
	DEBUG("release resource finish");
}
//----------------------------------------------------------------------

void cbexit()
{
	release_resource();
	char checker_filename[255] = {0};
	if (tsp_pid) {
		::sprintf(checker_filename, TSP_CHECKER_TEMPLETE, tsp_pid);
		if (::access(checker_filename, F_OK) == 0) {
			::unlink(checker_filename);
			DEBUG("checker_file unlink ok - [%s]", checker_filename);
		}
	}
}
//----------------------------------------------------------------------

inline int streaming_write(const char *buffer, size_t buffer_len, bool enable_log = false)
{
	if (enable_log) {
		DEBUG("response data :\n%s", buffer);
	}
	return write(1, buffer, buffer_len);
}
//----------------------------------------------------------------------

#define DD_LOG(X,...) { \
		char log_message[128] = {0};\
		sprintf(log_message, "echo \"" X "\" > /tmp/tsp_checker.log", ##__VA_ARGS__);\
		system(log_message);\
	}
//----------------------------------------------------------------------

inline int exist_process(pid_t pid)
{
	char process_path[255] = {0};
	sprintf(process_path, "/proc/%d", pid);
	if (access(process_path, F_OK) == 0)
		return 1;
	return 0;
}
//----------------------------------------------------------------------

inline int send_signal(pid_t pid, int signal)
{
	if (exist_process(pid)) {
		kill(pid, signal);
		DD_LOG("  >> run kill-pid : %ld -> %ld (%d)", getpid(), pid, signal);
	}
	return 0;
}
//----------------------------------------------------------------------

void signal_handler_checker(int sig_no)
{
	release_webif_record();
	is_terminated = true;
}
//----------------------------------------------------------------------

int check_sleep(pid_t pid, int sleep_time)
{
	int sleep_count = 0;
	int max_sleep_count = sleep_time * 10;

	while (sleep_count++ < max_sleep_count) {
		if (!exist_process(pid)) {
			return 0;
		}
		usleep(100*1000);
	}
	return 1;
}
//----------------------------------------------------------------------

int tsp_checker(pid_t pid)
{
	char check_filename[255] = {0};
	
	sprintf(check_filename, TSP_CHECKER_TEMPLETE, ::getppid());

	int timebase_count = 0, exit_count = 0;
	while(!is_terminated) {
		if (timebase_count != 10) {
			timebase_count++;
		}
		else {
			if (access(check_filename, F_OK) != 0) {
				send_signal(tsp_pid, SIGUSR2);
				DD_LOG("no found %s, %d", check_filename, timebase_count);
				break;
			}
		}

		struct stat sb;
		stat(check_filename, &sb);

		if (last_updated_time == sb.st_ctime && timebase_count == 10) {
			if (exit_count > 2) {
				send_signal(tsp_pid, SIGUSR2);
				DD_LOG("%ld == %ld", last_updated_time, sb.st_ctime);
				break;
			}
			exit_count++;
			sleep(1);
			continue;
		}
		exit_count = 0;
		last_updated_time = sb.st_ctime;
		sleep(1);
	}
	unlink(check_filename);

	DD_LOG("kill (%ld)", tsp_pid);

	if (check_sleep(tsp_pid, 3)) {
		send_signal(tsp_pid, SIGKILL);
		check_sleep(tsp_pid, 2);
	}
	return 0;
}
//----------------------------------------------------------------------

int main(int argc, char **argv)
{
	if (argc > 1) {
		if (strcmp(argv[1], "-h") == 0)
			show_help();
		exit(0);
	}
	tsp_pid = ::getpid();
	
	signal(SIGUSR1, signal_handler_checker);

	Logger::instance()->init("/tmp/transtreamproxy", Logger::ERROR);
	signal(SIGINT,  signal_handler);
	signal(SIGSEGV, signal_handler);
	signal(SIGUSR2, signal_handler);
	signal(SIGPIPE, signal_handler);

	atexit(cbexit);

	is_terminated = false;

	char update_status_command[255] = {0};


	HttpHeader header;
	std::string req = HttpHeader::read_request();

	DEBUG("request head :\n%s", req.c_str());

	try {
		if (req.find("\r\n\r\n") == std::string::npos) {
			throw(http_trap("no found request done code.", 400, "Bad Request"));
		}

		if (header.parse_request(req) == false) {
			throw(http_trap("request parse error.", 400, "Bad Request"));
		}

		if (header.method != "GET") {
			throw(http_trap("not support request type.", 400, "Bad Request, not support request"));
		}

		int video_pid = 0, audio_pid = 0, pmt_pid = 0;

		switch(header.type) {
		case HttpHeader::TRANSCODING_FILE:
			try {
				std::string uri = UriDecoder().decode(header.page_params["file"].c_str());
				encoder = new Encoder();
				source = new Mpeg(uri, false);
			}
			catch (const trap &e) {
				throw(http_trap(e.what(), 404, "Not Found"));
			}
			break;
		case HttpHeader::TRANSCODING_LIVE:
			try {
				checker_pid = ::fork();
				if (checker_pid == 0) {
					tsp_checker(checker_pid);
					exit(0);
				}

				sprintf(update_status_command, "touch " TSP_CHECKER_TEMPLETE, tsp_pid);
				system(update_status_command);

				encoder = new Encoder();
				source = new Demuxer(&header);
			}
			catch (const http_trap &e) {
				throw(e);
			}
			break;
		case HttpHeader::TRANSCODING_FILE_CHECK:
		case HttpHeader::M3U:
			try {
				std::string response = header.build_response((Mpeg*) source);
				if (response != "") {
					streaming_write(response.c_str(), response.length(), true);
				}
			}
			catch (...) {
			}
			exit(0);
 		case HttpHeader::TRANSCODING_LIVE_STOP: {
				char command[32] = {0};
				sprintf(command, "kill -16 %d", SessionMap::get()->get_pid_by_ip(Util::host_addr()));
				system(command); /* sending SIGUSR1 signal to specific transtreamproxy process before zapping another tp. */ 
				DEBUG("---->> live stop starting at %s :: %s <<----", get_timestamp(), command);
				throw(http_trap(std::string("transcoding live stop : ") + Util::ultostr(header.type), 200, "OK"));
			}
		default:
			throw(http_trap(std::string("not support source type : ") + Util::ultostr(header.type), 400, "Bad Request"));
		}

		pmt_pid   = source->pmt_pid;
		video_pid = source->video_pid;
		audio_pid = source->audio_pid;

		if (!encoder->retry_open(2, 3)) {
			throw(http_trap("encoder open fail.", 503, "Service Unavailable"));
		}

		if (encoder->state == Encoder::ENCODER_STAT_OPENED) {
			std::string response = header.build_response((Mpeg*) source);
			if (response == "") {
				throw(http_trap("response build fail.", 503, "Service Unavailable"));
			}

			streaming_write(response.c_str(), response.length(), true);

			if (header.type == HttpHeader::TRANSCODING_FILE) {
				((Mpeg*) source)->seek(header);
			}

			if (source->is_initialized()) {
				if (!encoder->ioctl(Encoder::IOCTL_SET_VPID, video_pid)) {
					throw(http_trap("video pid setting fail.", 503, "Service Unavailable"));
				}
				if (!encoder->ioctl(Encoder::IOCTL_SET_APID, audio_pid)) {
					throw(http_trap("audio pid setting fail.", 503, "Service Unavailable"));
				}

				if (pmt_pid != -1) {
					if (!encoder->ioctl(Encoder::IOCTL_SET_PMTPID, pmt_pid)) {
						throw(http_trap("pmt pid setting fail.", 503, "Service Unavailable"));
					}
				}
			}
		}

		if (header.type == HttpHeader::TRANSCODING_LIVE) {
			((Demuxer*)source)->open();
			if (((Demuxer*)source)->get_fd() < 0) {
				throw(http_trap("demux open fail!!", 503, "Service Unavailable"));
			}
		}
		source_thread_id = pthread_create(&source_thread_handle, 0, source_thread_main, 0);
		if (source_thread_id < 0) {
			is_terminated = true;
			throw(http_trap("souce thread create fail.", 503, "Service Unavailable"));
		}
		else {
			pthread_detach(source_thread_handle);
			if (!source->is_initialized()) {
				sleep(1);
			}

			if (!encoder->ioctl(Encoder::IOCTL_START_TRANSCODING, 0)) {
				is_terminated = true;
				throw(http_trap("start transcoding fail.", 503, "Service Unavailable"));
			}
			else {
				stream_thread_id = pthread_create(&stream_thread_handle, 0, streaming_thread_main, 0);
				if (stream_thread_id < 0) {
					is_terminated = true;
					throw(http_trap("stream thread create fail.", 503, "Service Unavailable"));
				}
			}
		}

		while(!is_terminated) {
			system(update_status_command);
			sleep(1);
		}

		send_signal(checker_pid, SIGUSR1);
		pthread_join(stream_thread_handle, 0);

		is_terminated = true;

		if (source != 0) {
			delete source;
			source = 0;
		}
	}
	catch (const http_trap &e) {
		ERROR("%s", e.message.c_str());
		std::string error = "";
		if (e.http_error == 401 && header.authorization.length() > 0) {
			error = header.authorization;
		}
		else {
			error = HttpUtil::http_error(e.http_error, e.http_header);
		}
		streaming_write(error.c_str(), error.length(), true);
		send_signal(checker_pid, SIGUSR1);
		exit(-1);
	}
	catch (...) {
		ERROR("unknown exception...");
		std::string error = HttpUtil::http_error(400, "Bad request");
		streaming_write(error.c_str(), error.length(), true);
		send_signal(checker_pid, SIGUSR1);
		exit(-1);
	}
	send_signal(checker_pid, SIGUSR1);

	DEBUG("---->> done at %s <<----", get_timestamp());
	return 0;
}
//----------------------------------------------------------------------

void *streaming_thread_main(void *params)
{
	if (is_terminated) return 0;

	INFO("streaming thread start.");

	try {
		unsigned char buffer[BUFFFER_SIZE];

		while(!is_terminated) {
			int rc = 0, wc = 0;
			struct pollfd poll_fd[2];
			poll_fd[0].fd = encoder->get_fd();
			poll_fd[0].events = POLLIN | POLLHUP;

			int poll_state = ::poll(poll_fd, 1, 500);
			if (poll_state == -1) {
				throw(trap("poll error."));
			}
			if (poll_fd[0].revents & POLLIN) {
				rc = wc = 0;
				rc = ::read(encoder->get_fd(), buffer, BUFFFER_SIZE - 1);

				//DEBUG("%d bytes read..", rc);

				if (rc <= 0) {
					continue;
				}
				else {
					wc = streaming_write((const char*) buffer, rc);
					if (wc < rc) {
						//DEBUG("need rewrite.. remain (%d)", rc - wc);
						int retry_wc = 0;
						for (int remain_len = rc - wc; (rc != wc) && (!is_terminated); remain_len -= retry_wc) {
							if (is_terminated) {
								throw(trap("terminated"));
							}
							retry_wc = streaming_write((const char*) (buffer + rc - remain_len), remain_len);
							wc += retry_wc;
						}
						LOG("re-write result : %d - %d", wc, rc);
					}
				}
			}
			else if (poll_fd[0].revents & POLLHUP) {
				if (encoder->state == Encoder::ENCODER_STAT_STARTED) {
					DEBUG("stop transcoding..");
					encoder->ioctl(Encoder::IOCTL_STOP_TRANSCODING, 0);
				}
				break;
			}
			usleep(0);
		}
	}
	catch (const trap &e) {
		ERROR("%s %s (%d)", e.what(), ::strerror(errno), errno);
	}
	is_terminated = true;
	INFO("streaming thread stop.");

	pthread_exit(0);

	return 0;
}
//----------------------------------------------------------------------

void *source_thread_main(void* params)
{

	INFO("source thread start.");

	try {
		int poll_state, rc, wc;
		struct pollfd poll_fd_enc[1];
		struct pollfd poll_fd_src[1];
		unsigned char buffer[BUFFFER_SIZE];

		poll_fd_enc[0].fd = encoder->get_fd();
		poll_fd_enc[0].events = POLLOUT;
		poll_fd_src[0].fd = source->get_fd();
		poll_fd_src[0].events = POLLIN;

		while(!is_terminated) {
			poll_state = poll(poll_fd_src, 1, 500);
			if (poll_state == -1) {
				throw(trap("poll error."));
			}
			if (poll_fd_src[0].revents & POLLIN) {
				rc =::read(source->get_fd(), buffer, BUFFFER_SIZE - 1);
				if (rc <= 0) {
					 continue;
				}
				else if (rc > 0) {
					poll_fd_enc[0].revents = 0;
					poll_state = poll(poll_fd_enc, 1, 500);
					if (poll_fd_enc[0].revents & POLLOUT) {
						wc = write(encoder->get_fd(), buffer, rc);
						if (wc < rc) {
							int retry_wc = 0;
							for (int remain_len = rc - wc; (rc != wc) && (!is_terminated); remain_len -= retry_wc) {
								if (is_terminated) {
									throw(trap("terminated"));
								}
								poll_fd_enc[0].revents = 0;
								poll_state = poll(poll_fd_enc, 1, 500);
								if (poll_state == -1) {
									throw(trap("poll error."));
								}

								if (poll_fd_enc[0].revents & POLLOUT) {
									retry_wc = ::write(encoder->get_fd(), (buffer + rc - remain_len), remain_len);
									wc += retry_wc;
								}
								LOG("re-write result : %d - %d", wc, rc);
							}
						}
					}
				}
			}
			usleep(0);
		}
	}
	catch (const trap &e) {
		ERROR("%s %s (%d)", e.what(), ::strerror(errno), errno);
	}
	INFO("source thread stop.");

    pthread_exit(0);

	return 0;
}
//----------------------------------------------------------------------

void signal_handler(int sig_no)
{
	ERROR("signal no : %s (%d)", strsignal(sig_no), sig_no);
	is_terminated = true;
}
//----------------------------------------------------------------------

void show_help()
{
	printf("usage : transtreamproxy [-h]\n");
	printf("\n");
	printf(" * To active debug mode, input NUMBER on /tmp/debug_on file. (default : warning)\n");
	printf("   NUMBER : error(1), warning(2), info(3), debug(4), log(5)\n");
	printf("\n");
	printf(" ex > echo \"4\" > /tmp/.debug_on\n");
}
//----------------------------------------------------------------------

