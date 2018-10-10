#include <chrono>
#include <windows.h>
#include <shellapi.h>

#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Progress.H>
#include <FL/fl_ask.H>
#include <FL/x.H>

#include <boost/filesystem.hpp>
#include <fmt/format.h>

#include "cli-parser.hpp"
#include "update-client.hpp"

namespace fs = boost::filesystem;
namespace chrono = std::chrono;

using chrono::high_resolution_clock;
using chrono::duration_cast;

/* Because Windows doesn't provide us a Unicode
 * command line by default and the command line
 * it does provide us is in UTF-16LE. */
struct MultiByteCommandLine {
	MultiByteCommandLine();
	~MultiByteCommandLine();

	MultiByteCommandLine(const MultiByteCommandLine&) = delete;
	MultiByteCommandLine(MultiByteCommandLine&&) = delete;
	MultiByteCommandLine &operator=(const MultiByteCommandLine&) = delete;
	MultiByteCommandLine &operator=(MultiByteCommandLine&&) = delete;

	LPSTR *argv() { return m_argv; };
	int argc() { return m_argc; };

private:
	int m_argc{0};
	LPSTR *m_argv{nullptr};
};

MultiByteCommandLine::MultiByteCommandLine()
{
	LPWSTR lpCommandLine = GetCommandLineW();
	LPWSTR *wargv = CommandLineToArgvW(lpCommandLine, &m_argc);
	m_argv = new LPSTR[m_argc];

	/* The glory of using an unreliably sized byte
	 * type to hold a 2-byte codepoint that practically
	 * no one in the universe wants to actaully use.
	 * If there's one thing I'd like Microsoft to
	 * change, it's their shitty integration with
	 * UTF-16LE (or UCS-2 as they call it).
	 * Anyways... this converts each UTF-16LE string
	 * to UTF-8 (or something it understands). */
	for (int i = 0; i < m_argc; ++i) {
		BOOL bUsedDefaultChar;

		DWORD size = WideCharToMultiByte(
			CP_UTF8, 0,
			wargv[i], -1,
			NULL, 0,
			NULL,
			&bUsedDefaultChar
		);

		m_argv[i] = new CHAR[size];

		size = WideCharToMultiByte(
			CP_UTF8, 0,
			wargv[i], -1,
			m_argv[i], size,
			NULL,
			&bUsedDefaultChar
		);
	}

	LocalFree(wargv);
}

MultiByteCommandLine::~MultiByteCommandLine()
{
	for (int i = 0; i < m_argc; ++i) {
		delete m_argv[i];
	}

	delete m_argv;
}

BOOL StartApplication(LPWSTR lpCommandLine, LPCWSTR lpWorkingDirectory)
{
	BOOL bSuccess;
	DWORD dwShellProcId;
	HANDLE hShellProc;
	HANDLE hShellToken;
	HANDLE hNewToken;
	STARTUPINFO xStartupInfo = { 0 };
	PROCESS_INFORMATION xProcInfo = { 0 };
	HWND hwndShell = GetShellWindow();

	if (hwndShell == NULL)
		return FALSE;

	GetWindowThreadProcessId(hwndShell, &dwShellProcId);

	hShellProc = OpenProcess(
		PROCESS_QUERY_INFORMATION,
		FALSE, dwShellProcId
	);

	if (hShellProc == NULL)
		return FALSE;

	bSuccess = OpenProcessToken(hShellProc, TOKEN_DUPLICATE, &hShellToken);

	if (bSuccess == 0)
		return FALSE;

	bSuccess = DuplicateTokenEx(
		hShellToken,
		TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY |
		TOKEN_DUPLICATE | TOKEN_ADJUST_DEFAULT |
		TOKEN_ADJUST_SESSIONID,
		NULL,
		SecurityImpersonation,
		TokenPrimary,
		&hNewToken
	);

	if (bSuccess == 0)
		return FALSE;

	return CreateProcessWithTokenW(
		hNewToken,
		0, NULL,
		lpCommandLine,
		0, NULL,
		lpWorkingDirectory,
		&xStartupInfo,
		&xProcInfo
	);
}

static LPWSTR ConvertToUtf16(const char *from, int *from_size)
{
	int to_size = MultiByteToWideChar(
		CP_UTF8, 0,
		from, *from_size,
		NULL, 0
	);

	auto to = new WCHAR[to_size];

	int size = MultiByteToWideChar(
		CP_UTF8, 0,
		from, *from_size,
		to, to_size
	);

	if (size == 0) {
		*from_size = 0;
		return NULL;
	}

	*from_size = to_size;

	return to;
}

BOOL StartApplication(const char *lpCommandLine, const char *lpWorkingDir)
{
	/* Convert UTF-8 command line back to UTF-16 */
	int dwCLSize = -1;
	LPWSTR lpWideCommandLine = ConvertToUtf16(lpCommandLine, &dwCLSize);
	LPWSTR lpWideWorkingDir;
	BOOL bSuccess = false;

	dwCLSize = -1;

	lpWideWorkingDir = ConvertToUtf16(lpWorkingDir, &dwCLSize);

	bSuccess = StartApplication(lpWideCommandLine, lpWideWorkingDir);

	delete lpWideCommandLine;
	delete lpWideWorkingDir;

	return bSuccess;
}

struct callbacks_impl :
	public
	  client_callbacks,
	  downloader_callbacks,
	  updater_callbacks,
	  pid_callbacks
{
	int x = 300;
	int y = 100;
	Fl_Window *frame{nullptr}; /* Toplevel window */
	Fl_Progress* progress_worker{nullptr};
	std::atomic_uint files_done{0};
	std::vector<size_t> file_sizes{0};
	size_t num_files{0};
	int num_workers{0};
	size_t total_accum{0};
	high_resolution_clock::time_point start_time;

	callbacks_impl(const callbacks_impl&) = delete;
	callbacks_impl(const callbacks_impl&&) = delete;
	callbacks_impl &operator=(const callbacks_impl&) = delete;
	callbacks_impl &operator=(callbacks_impl&&) = delete;

	callbacks_impl()
	{
		frame = new Fl_Double_Window(x, y);

		frame->label("Streamlabs OBS Updater");

		/* 23, 36, 45 - Streamlabs Gray */
		frame->color(fl_rgb_color(23, 36, 45));

		frame->end();
		frame->show();

		HWND hw = fl_xid(frame);

		ULONG_PTR style = GetClassLongPtr(hw, GCL_STYLE);
		SetClassLongPtr(hw, GCL_STYLE, style | CS_NOCLOSE);
	}

	static void kill_impl(void *data)
	{
		auto *impl = static_cast<callbacks_impl*>(data);
		delete impl->frame;
	}

	~callbacks_impl() { }

	void initialize() final
	{
		frame->begin();

		/* For whatever reason, we're given 10
		 * pixels of buffer space on position
		 * already. Can't figure out why. So
		 * setting this to 10 will actually
		 * set position to 20. */
		int x_pos = 10;
		int y_size = 20;
		int x_size = x - 20;
		int y_pos = (y / 2);

		progress_worker =
			new Fl_Progress(x_pos, y_pos, x_size, y_size);

		/* 49, 195, 162 - Streamlabs Green */
		progress_worker->color(FL_WHITE, fl_rgb_color(49, 195, 162));
		progress_worker->minimum(0.f);
		progress_worker->maximum(1.f);
		progress_worker->copy_label("Looking for new files...");

		frame->end();
	}

	void success() final
	{
		Fl::awake(kill_impl, this);
	}

	void error() final
	{
		Fl::awake(kill_impl, this);

	}

	void downloader_start(int num_threads, size_t num_files_) final
	{
		file_sizes.resize(num_threads, 0);
		this->num_files = num_files_;
		start_time = high_resolution_clock::now();
	}

	void download_file(
	  int thread_index,
	  std::string &relative_path,
	  size_t size
	) final {
		/* Our specific UI doesn't care when we start, we only
		 * care when we're finished. A more technical UI could show
		 * what each thread is doing if they so wanted. */
		file_sizes[thread_index] = size;
	}

	void download_progress(
	  int thread_index,
	  size_t consumed,
	  size_t accum
	) final {
		total_accum += consumed;

		high_resolution_clock::time_point current_time =
			high_resolution_clock::now();

		/* Convert bytes to megabytes */
		double average = total_accum * 0.000001;

		/* We calculate against nanoseconds to give a more accurate read,
		 * otherwise we'd have lots of variance. Here we just multiply
		 * the result to take into account that we use nanoseconds and not
		 * seconds */
		auto time_passed = current_time - start_time;

		auto time_passed_ns =
			duration_cast<std::chrono::nanoseconds>(time_passed);

		average /= time_passed_ns.count();
		average *= 1000000000.0;

		/* Note that the above takes an average over the period of time the
		 * downloader started. This means that if bandwidth suddenly changes
		 * towards the end of the updater, it won't seem to make much difference.
		 * For now, I think this is alright but could be improved. */

		/* We don't currently show per-file progress but we could
		 * progress the bar based on files_done + remainder of
		 * all in-progress files done. */
		const char *label_format = "Downloading {} of {} - {:.2f} MB\\s";

		if (accum != file_sizes[thread_index]) {
			std::string label = fmt::format(
				label_format, files_done,
				num_files, average
			);

			Fl::lock();
			progress_worker->copy_label(label.c_str());
			Fl::unlock();
			Fl::awake();

			return;
		}

		++files_done;

		float percent = (float)files_done / (float)num_files;

		std::string label = fmt::format(
			label_format, files_done,
			num_files, average
		);

		Fl::lock();
		progress_worker->copy_label(label.c_str());
		progress_worker->value(percent);
		Fl::unlock();
		Fl::awake();
	}

	void download_worker_finished(int thread_index) final
	{
	}

	void downloader_complete() final
	{
	}

	void pid_start() final
	{
		Fl::lock();

		progress_worker->label("Waiting on application to exit...");
		progress_worker->value(0.f);

		Fl::unlock();
		Fl::awake();
	}

	void pid_waiting_for(uint64_t pid) final { }
	void pid_wait_finished(uint64_t pid) final { }
	void pid_wait_complete() final { }

	void updater_start() final { };
	void update_file(std::string &filename) final { }
	void update_finished(std::string &filename) final { }
	void updater_complete() final { }
};

extern "C"
int wWinMain(
  HINSTANCE hInstance,
  HINSTANCE hPrevInstance,
  LPWSTR    lpCmdLineUnused,
  int       nCmdShow
) {
	Fl::scheme("gleam");
	Fl::lock();

	struct update_parameters params;
	callbacks_impl cb_impl;

	MultiByteCommandLine command_line;

	bool success = su_parse_command_line(
		command_line.argc(),
		command_line.argv(),
		&params
	);

	if (!success) {
		fl_message("Failed to parse CLI arguments!");
		return 0;
	}

	auto client_deleter = [] (struct update_client *client) {
		destroy_update_client(client);
	};

	std::unique_ptr<struct update_client, decltype(client_deleter)>
		client(create_update_client(&params), client_deleter);

	update_client_set_client_events(client.get(), &cb_impl);
	update_client_set_downloader_events(client.get(), &cb_impl);
	update_client_set_updater_events(client.get(), &cb_impl);
	update_client_set_pid_events(client.get(), &cb_impl);
	update_client_start(client.get());

	Fl::run();

	update_client_flush(client.get());

	Fl::unlock();

#define THERE_OR_NOT(x) \
	((x).empty() ? NULL : (x).c_str())

	success = StartApplication(
		THERE_OR_NOT(params.exec),
		THERE_OR_NOT(params.exec_cwd)
	);

	if (!success) {
		fl_message(
			"Failed to restart application!\n"
			"Just manually start it. Sorry about that!"
		);
	}
#undef THERE_OR_NOT

	return 0;
}