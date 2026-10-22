//
//	License type: BSD 3-Clause License
//	License copy: https://github.com/Telecominfraproject/wlan-cloud-ucentralgw/blob/master/LICENSE
//
// Diagnostic probe tests for potential Kafka callback deadlock (P2 reviewer request).
//
// Background:
//   KafkaConsumer::run() dispatches registered callbacks while holding ConsumerMutex_.
//   Both RegisterTopicWatcher() and UnregisterTopicWatcher() also acquire ConsumerMutex_.
//
//   This creates two locking scenarios that must be characterized before any locking change:
//
//   1. Re-entrant callback self-deadlock probe:
//      A callback on the run() thread holds ConsumerMutex_. If that callback calls
//      UnregisterTopicWatcher() on the same consumer, it tries to re-lock the same
//      non-recursive std::mutex -> deadlock.
//      Probe design: child process signals parent via pipe when callback enters
//      UnregisterTopicWatcher(). Parent enforces a bounded 10-second timeout.
//      If unregistration returns, child signals 'R'. If it deadlocks, parent kills
//      child with SIGKILL and reports observation consistent with self-deadlock.
//
//   2. Concurrent unregistration and shutdown probe:
//      Thread A holds ConsumerMutex_ while dispatching a long-running callback.
//      Thread B calls UnregisterTopicWatcher().
//      Thread C calls consumer.Stop() while the callback is still actively blocked.
//      Probe design: runs in a separate child process with a parent-enforced 65-second
//      watchdog timeout. Tests unregistration and shutdown overlapping with an active
//      callback, releases callback gate, and verifies clean resolution and shutdown.
//
// Phasing:
//   - Phase 1 (Diagnostic probe — current): Reproduces and reports the deadlock behavior
//     without failing CI, satisfying reviewer requirement to observe before modifying code.
//   - Phase 2 (Regression test — after fix): Once locking is modernized to dispatch callbacks
//     outside ConsumerMutex_, Test 1 will require NO DEADLOCK to pass.
//
// Exit codes:
//   - 77: SKIPPED (KAFKA_BROKERLIST not set)
//   -  0: Probes executed and reported findings
//   -  1: Test failure (unexpected crash, message delivery failure, hang on shutdown)
//

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <future>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <thread>

// POSIX process / signal / polling headers (Linux)
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "Poco/ConsoleChannel.h"
#include "Poco/UUIDGenerator.h"
#include "framework/KafkaManager.h"
#include "framework/KafkaTopics.h"

// ---------------------------------------------------------------------------
// Exit codes
// ---------------------------------------------------------------------------
static constexpr int EXIT_SKIP = 77;   // intentional skip (no broker)
static constexpr int EXIT_FAIL = 1;
static constexpr int EXIT_PASS = 0;

// ---------------------------------------------------------------------------
// Macro helpers
// ---------------------------------------------------------------------------
#define TEST_ASSERT(cond, msg)                                                  \
	do {                                                                        \
		if (!(cond)) {                                                          \
			std::cerr << "TEST FAILURE [" << __FILE__ << ":" << __LINE__        \
			          << "]: " << (msg) << std::endl;                           \
			std::exit(EXIT_FAIL);                                               \
		}                                                                       \
	} while (0)

// ---------------------------------------------------------------------------
// Stubs (identical pattern to KafkaConsumerRoles_test.cpp)
// ---------------------------------------------------------------------------
namespace OpenWifi {

	static std::string g_MockGroupId        = "prov";
	static std::string g_MockClientId       = "prov1";
	static std::string g_MockInstanceId     = "owprov-1-550e8400-e29b-41d4-a716-446655440000";
	static std::string g_MockPrivateEndPoint = "https://owprov-internal:17005";
	static uint64_t    g_MockSystemId       = 12345;

	std::string MicroServiceConfigGetString(const std::string &Key,
	                                        const std::string &DefaultValue) {
		if (Key == "openwifi.kafka.group.id") return g_MockGroupId;
		if (Key == "openwifi.kafka.client.id") return g_MockClientId;
		if (Key == "openwifi.kafka.brokerlist") {
			const char *e = std::getenv("KAFKA_BROKERLIST");
			return (e && *e) ? std::string(e) : DefaultValue;
		}
		return DefaultValue;
	}

	bool MicroServiceConfigGetBool(const std::string &, bool DefaultValue) {
		return DefaultValue;
	}

	std::uint64_t MicroServiceConfigGetInt(const std::string &, std::uint64_t DefaultValue) {
		return DefaultValue;
	}

	std::string MicroServiceRuntimeInstanceId() { return g_MockInstanceId; }
	std::string MicroServicePrivateEndPoint()   { return g_MockPrivateEndPoint; }
	std::uint64_t MicroServiceID()              { return g_MockSystemId; }

	SubSystemServer::SubSystemServer(const std::string &Name,
	                                 const std::string &LoggingPrefix,
	                                 const std::string &SubSystemConfigPrefix)
		: Name_(Name), LoggerPrefix_(LoggingPrefix),
		  SubSystemConfigPrefix_(SubSystemConfigPrefix) {
		Logger_ = std::make_unique<LoggerWrapper>(Poco::Logger::root());
	}
	void SubSystemServer::initialize(Poco::Util::Application &) {}

} // namespace OpenWifi

using namespace OpenWifi;

// ---------------------------------------------------------------------------
// Helper: poll a pipe fd for a single token byte with bounded timeout and EINTR handling
// ---------------------------------------------------------------------------
static bool read_pipe_token(int fd, char &out_token, int timeout_ms) {
	struct pollfd pfd;
	pfd.fd = fd;
	pfd.events = POLLIN;
	pfd.revents = 0;

	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	while (true) {
		auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
			deadline - std::chrono::steady_clock::now()).count();
		if (remaining <= 0) break;

		int ret = ::poll(&pfd, 1, static_cast<int>(remaining));
		if (ret > 0 && (pfd.revents & POLLIN)) {
			ssize_t bytes = ::read(fd, &out_token, 1);
			return (bytes == 1);
		}
		if (ret < 0 && errno == EINTR) {
			continue; // Retry if interrupted by signal
		}
		break;
	}
	return false;
}

// ---------------------------------------------------------------------------
// Test 1: Re-entrant Callback Self-Deadlock Probe
//
// Design:
//   1. Parent creates a unidirectional pipe and forks a child process.
//   2. Child sets up a GroupConsumer, registers a callback on topic
//      test_reentrant_deadlock_probe, and starts the consumer.
//   3. Child filters messages by unique payload to ignore stale messages from
//      previous runs, and loops producing messages until the callback triggers.
//   4. When the callback enters:
//      - It writes 'E' (Entered) to the pipe.
//      - It immediately invokes UnregisterTopicWatcher() on its own consumer.
//      - If UnregisterTopicWatcher() returns, it writes 'R' (Returned) to the pipe.
//      - If an exception is thrown, it writes 'X' and immediately calls ::_exit(3).
//   5. Parent receives 'E', then enforces a bounded 10-second timeout for 'R'.
//      - If 'R' is received -> NO DEADLOCK observed.
//      - If timeout expires with child still running -> reports observation
//        consistent with ConsumerMutex_ self-deadlock; kills child with SIGKILL.
//      - If child crashes / exits abnormally -> report diagnostic failure.
// ---------------------------------------------------------------------------

[[noreturn]] static void reentrant_child_body(int write_fd, const std::string &topic) {
	Poco::AutoPtr<Poco::ConsoleChannel> ch(new Poco::ConsoleChannel);
	Poco::Logger::root().setChannel(ch);
	Poco::Logger::root().setLevel(Poco::Message::PRIO_ERROR);

	std::string token = Poco::UUIDGenerator::defaultGenerator().createRandom().toString();
	g_MockGroupId    = "deadlock-child-" + token;
	g_MockClientId   = "deadlock-client-" + token;
	g_MockInstanceId = "owprov-dlchild-" + token;

	const std::string expected_payload = "reentrant-probe-" + token;

	auto &consumer = KafkaManager()->GroupConsumer();

	std::atomic<bool> callback_entered{false};
	std::atomic<bool> unregister_returned{false};
	std::atomic<uint64_t> registered_id{0};

	Types::TopicNotifyFunction cb = [&](const std::string &, const std::string &msg) {
		// Filter by unique payload so stale messages from prior runs are ignored
		if (msg != expected_payload) return;

		callback_entered.store(true);

		// Signal to parent that callback has entered and is about to call Unregister
		char c = 'E';
		(void)::write(write_fd, &c, 1);

		try {
			// Attempt re-entrant unregistration while run() holds ConsumerMutex_
			KafkaManager()->UnregisterTopicWatcher(topic, registered_id.load(),
			                                       KafkaConsumerType::Group);
			unregister_returned.store(true);

			// Signal to parent that Unregister returned successfully
			c = 'R';
			(void)::write(write_fd, &c, 1);
		} catch (const std::exception &) {
			c = 'X';
			(void)::write(write_fd, &c, 1);
			::close(write_fd);
			::_exit(3); // Exit immediately to avoid hanging child or parent
		} catch (...) {
			c = 'X';
			(void)::write(write_fd, &c, 1);
			::close(write_fd);
			::_exit(3); // Exit immediately to avoid hanging child or parent
		}
	};

	registered_id.store(
		KafkaManager()->RegisterTopicWatcher(topic, cb, KafkaConsumerType::Group));
	consumer.Start();
	TEST_ASSERT(consumer.IsRunning(), "Child GroupConsumer must be running");

	// Produce messages repeatedly until callback triggers (up to 20 s)
	KafkaProducer producer;
	producer.Start();

	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	while (!callback_entered.load() && std::chrono::steady_clock::now() < deadline) {
		producer.Produce(topic.c_str(), "reentrant-key", expected_payload);
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
	}

	if (!callback_entered.load()) {
		char c = 'F';
		(void)::write(write_fd, &c, 1);
		::close(write_fd);
		std::exit(2);
	}

	// Main child thread waits for callback to finish unregistration if it returns
	auto unreg_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	while (!unregister_returned.load() && std::chrono::steady_clock::now() < unreg_deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	if (unregister_returned.load()) {
		consumer.Stop();
		producer.Stop();
		::close(write_fd);
		std::exit(0);
	}

	// If Unregister did not return (deadlock on mutex), wait for parent to kill us
	while (true) {
		pause();
	}
}

static void test_reentrant_callback_self_deadlock_probe() {
	std::cout << "=== Test 1: Re-entrant Callback Self-Deadlock Probe ===" << std::endl;

	const char *broker_env = std::getenv("KAFKA_BROKERLIST");
	const std::string broker = broker_env ? broker_env : "";
	const std::string topic  = "test_reentrant_deadlock_probe";

	std::cout << "  Broker: " << broker << "  Topic: " << topic << std::endl;

	int pipefd[2];
	if (::pipe(pipefd) != 0) {
		std::cerr << "  ERROR: pipe() failed: " << strerror(errno) << std::endl;
		std::exit(EXIT_FAIL);
	}

	std::cout << "  Forking child process; waiting for callback entry signal..." << std::endl;

	pid_t child = fork();
	TEST_ASSERT(child >= 0, "fork() failed");

	if (child == 0) {
		::close(pipefd[0]); // close read end in child
		reentrant_child_body(pipefd[1], topic);
		// noreturn
	}

	::close(pipefd[1]); // close write end in parent

	// Phase 1: Wait up to 25 seconds for token 'E' (callback entered)
	char token = 0;
	bool got_entered = read_pipe_token(pipefd[0], token, 25000);

	if (!got_entered || token != 'E') {
		int status = 0;
		pid_t ret = ::waitpid(child, &status, WNOHANG);
		if (ret == 0) {
			::kill(child, SIGKILL);
			::waitpid(child, &status, 0);
		}
		::close(pipefd[0]);

		if (token == 'F' || (WIFEXITED(status) && WEXITSTATUS(status) == 2)) {
			std::cerr << "  ERROR: Callback was not triggered by Kafka broker within 20 s." << std::endl;
		} else if (WIFSIGNALED(status)) {
			std::cerr << "  ERROR: Child process crashed with signal " << WTERMSIG(status)
			          << " before entering unregistration." << std::endl;
		} else {
			std::cerr << "  ERROR: Timeout waiting for callback entry signal from child." << std::endl;
		}
		std::exit(EXIT_FAIL);
	}

	std::cout << "  SIGNAL: Callback entered and initiated UnregisterTopicWatcher()." << std::endl;
	std::cout << "  Enforcing bounded 10 s timeout for UnregisterTopicWatcher() to return..." << std::endl;

	// Phase 2: Enforce bounded 10-second timeout for token 'R' (unregistration returned)
	bool got_returned = read_pipe_token(pipefd[0], token, 10000);

	if (got_returned && token == 'X') {
		int status = 0;
		bool reaped = false;
		for (int i = 0; i < 30; ++i) {
			pid_t ret = ::waitpid(child, &status, WNOHANG);
			if (ret == child) {
				reaped = true;
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
		if (!reaped) {
			::kill(child, SIGKILL);
			::waitpid(child, &status, 0);
		}
		::close(pipefd[0]);
		std::cerr << "  ERROR: UnregisterTopicWatcher() threw an unexpected exception inside callback." << std::endl;
		std::exit(EXIT_FAIL);
	}

	if (got_returned && token == 'R') {
		int status = 0;
		bool child_exited = false;
		for (int i = 0; i < 50; ++i) {
			pid_t ret = ::waitpid(child, &status, WNOHANG);
			if (ret == child) {
				child_exited = true;
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
		if (!child_exited) {
			::kill(child, SIGKILL);
			::waitpid(child, &status, 0);
			::close(pipefd[0]);
			std::cerr << "  ERROR: Child reported 'R' but did not exit within 5 s." << std::endl;
			std::exit(EXIT_FAIL);
		}
		::close(pipefd[0]);

		if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
			std::cout << "  RESULT: NO DEADLOCK — re-entrant Unregister returned successfully." << std::endl;
			std::cout << "  OBSERVED: Re-entrant callback did not deadlock." << std::endl;
			return;
		}
	}

	// If token 'R' was not received, check if child is still running (stuck in deadlock)
	int status = 0;
	pid_t ret = ::waitpid(child, &status, WNOHANG);

	if (ret == 0) {
		::kill(child, SIGKILL);
		::waitpid(child, &status, 0);
		::close(pipefd[0]);

		std::cout << "  OBSERVED: Callback entered; self-unregistration did not return within 10 seconds." << std::endl;
		std::cout << "  DIAGNOSIS: Consistent with ConsumerMutex_ self-deadlock." << std::endl;
		std::cout << "  EXPLANATION: KafkaManager.cpp line 314 locks ConsumerMutex_ during run() callback dispatch.\n"
		             "               KafkaManager.cpp line 383 unconditionally acquires ConsumerMutex_ in UnregisterTopicWatcher().\n"
		             "               Attempting self-unregistration locks the same non-recursive std::mutex -> permanent self-deadlock."
		          << std::endl;
		std::cout << "  RESULT: Self-deadlock reproduced as anticipated." << std::endl;
		return;
	}

	::close(pipefd[0]);

	if (WIFSIGNALED(status)) {
		std::cerr << "  ERROR: Child process terminated by signal " << WTERMSIG(status)
		          << " during unregistration." << std::endl;
		std::exit(EXIT_FAIL);
	} else {
		std::cerr << "  ERROR: Child process exited unexpectedly with status "
		          << WEXITSTATUS(status) << std::endl;
		std::exit(EXIT_FAIL);
	}
}

// ---------------------------------------------------------------------------
// Test 2: Concurrent Unregistration and Shutdown Probe
//
// Design:
//   - Runs in a separate child process with a parent-enforced 65-second watchdog
//     timeout to guarantee CI cannot hang even if unregister or shutdown deadlocks.
//   - Registers a watcher on GroupConsumer for topic test_concurrent_unregister_probe.
//   - Starts GroupConsumer and triggers callback via KafkaProducer with unique payload.
//   - Callback blocks on a condition variable gate, holding ConsumerMutex_ inside run().
//   - Thread B initiates UnregisterTopicWatcher() using a std::promise synchronization barrier.
//   - Thread C initiates consumer.Stop() while the callback is STILL actively blocked,
//     testing shutdown overlapping with an active callback.
//   - Observes unregistration and shutdown status before gate release.
//   - Releases the callback gate.
//   - Verifies that both unregistration and shutdown complete cleanly.
// ---------------------------------------------------------------------------
[[noreturn]] static void concurrent_child_body(const std::string &topic) {
	Poco::AutoPtr<Poco::ConsoleChannel> ch(new Poco::ConsoleChannel);
	Poco::Logger::root().setChannel(ch);
	Poco::Logger::root().setLevel(Poco::Message::PRIO_ERROR);

	std::string token = Poco::UUIDGenerator::defaultGenerator().createRandom().toString();
	g_MockGroupId    = "concurrent-test-" + token;
	g_MockClientId   = "concurrent-client-" + token;
	g_MockInstanceId = "owprov-concurrent-" + token;

	const std::string expected_payload = "concurrent-probe-" + token;

	std::mutex gate_mtx;
	std::condition_variable gate_cv;
	std::atomic<bool> callback_entered{false};
	std::atomic<bool> gate_open{false};
	std::atomic<uint64_t> watcher_id{0};

	Types::TopicNotifyFunction blocking_cb = [&](const std::string &, const std::string &msg) {
		// Filter by unique payload so stale messages from prior runs are ignored
		if (msg != expected_payload) return;

		callback_entered.store(true);
		// Block here — run() holds ConsumerMutex_ while waiting for gate
		std::unique_lock<std::mutex> lk(gate_mtx);
		gate_cv.wait(lk, [&] { return gate_open.load(); });
	};

	watcher_id.store(
		KafkaManager()->RegisterTopicWatcher(topic, blocking_cb, KafkaConsumerType::Group));
	TEST_ASSERT(watcher_id.load() > 0, "RegisterTopicWatcher must return non-zero ID");

	auto &consumer = KafkaManager()->GroupConsumer();
	consumer.Start();
	TEST_ASSERT(consumer.IsRunning(), "GroupConsumer must be running after Start()");

	KafkaProducer producer;
	producer.Start();

	std::cout << "  Producing messages until callback is entered..." << std::endl;
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	while (!callback_entered.load() && std::chrono::steady_clock::now() < deadline) {
		producer.Produce(topic.c_str(), "concurrent-key", expected_payload);
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
	}

	if (!callback_entered.load()) {
		{ std::lock_guard<std::mutex> lk(gate_mtx); gate_open.store(true); }
		gate_cv.notify_all();
		consumer.Stop();
		producer.Stop();
		std::cerr << "  ERROR: Callback was not triggered within 30 s." << std::endl;
		std::exit(EXIT_FAIL);
	}

	std::cout << "  Callback entered — callback is now executing and holding gate." << std::endl;

	// Launch unregistration on a second thread with explicit promise barrier
	std::promise<void> unregister_ready_prom;
	auto unregister_ready_fut = unregister_ready_prom.get_future();
	std::atomic<bool> unregister_completed{false};

	std::chrono::steady_clock::time_point t_start_unreg;
	std::chrono::steady_clock::time_point t_finish_unreg;

	std::thread unregister_thread([&]() {
		t_start_unreg = std::chrono::steady_clock::now();
		unregister_ready_prom.set_value();
		KafkaManager()->UnregisterTopicWatcher(topic, watcher_id.load(),
		                                       KafkaConsumerType::Group);
		t_finish_unreg = std::chrono::steady_clock::now();
		unregister_completed.store(true);
	});

	// Wait for unregister thread to reach invocation
	unregister_ready_fut.wait();

	// Observe whether thread waits during a 300 ms window while callback holds the gate
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	bool unregistration_blocked = !unregister_completed.load();
	TEST_ASSERT(unregistration_blocked,
		"UnregisterTopicWatcher() should block while the callback holds ConsumerMutex_");

	// Verify consumer is healthy before initiating shutdown
	TEST_ASSERT(consumer.IsRunning(),
		"Consumer terminated unexpectedly before explicit shutdown");

	// Start shutdown on a third thread while callback is STILL actively blocked
	std::promise<void> shutdown_ready_prom;
	auto shutdown_ready_fut = shutdown_ready_prom.get_future();
	std::atomic<bool> shutdown_completed{false};

	std::thread shutdown_thread([&]() {
		shutdown_ready_prom.set_value();
		consumer.Stop();
		shutdown_completed.store(true);
	});

	// Wait for shutdown thread to begin Stop()
	shutdown_ready_fut.wait();
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	bool shutdown_waiting_on_callback = !shutdown_completed.load();

	// Release the callback gate and record timestamp
	auto t_gate_release = std::chrono::steady_clock::now();
	{
		std::lock_guard<std::mutex> lk(gate_mtx);
		gate_open.store(true);
	}
	gate_cv.notify_all();

	// Verify unregistration completes safely within 10 s after gate is opened
	auto unreg_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (!unregister_completed.load() && std::chrono::steady_clock::now() < unreg_deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	TEST_ASSERT(unregister_completed.load(),
		"UnregisterTopicWatcher() must complete safely after callback finishes");
	unregister_thread.join();

	// Verify shutdown completes safely within 10 s after gate is opened
	auto shutdown_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (!shutdown_completed.load() && std::chrono::steady_clock::now() < shutdown_deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	TEST_ASSERT(shutdown_completed.load(),
		"consumer.Stop() must complete safely after callback finishes");
	shutdown_thread.join();

	TEST_ASSERT(!consumer.IsRunning(),
		"GroupConsumer must not be running after Stop()");

	// Report observed unregistration behavior
	if (unregistration_blocked) {
		auto blocked_duration_ms =
			std::chrono::duration_cast<std::chrono::milliseconds>(t_gate_release - t_start_unreg).count();
		auto completion_delay_ms =
			std::chrono::duration_cast<std::chrono::milliseconds>(t_finish_unreg - t_gate_release).count();
		std::cout << "  OBSERVED: UnregisterTopicWatcher() waited for " << blocked_duration_ms
		          << " ms while callback held gate, then completed in " << completion_delay_ms
		          << " ms after gate release (observation consistent with expected ConsumerMutex_ contention)." << std::endl;
	} else {
		auto elapsed_ms =
			std::chrono::duration_cast<std::chrono::milliseconds>(t_finish_unreg - t_start_unreg).count();
		std::cout << "  OBSERVED: UnregisterTopicWatcher() completed in " << elapsed_ms
		          << " ms without waiting for callback gate." << std::endl;
	}

	if (shutdown_waiting_on_callback) {
		std::cout << "  OBSERVED: Concurrent shutdown overlapped with blocked callback and resolved safely after gate release." << std::endl;
	}

	std::cout << "  VERIFIED: UnregisterTopicWatcher() completed safely." << std::endl;
	std::cout << "  VERIFIED: Consumer stopped cleanly." << std::endl;

	producer.Stop();

	std::cout << "  PASS: Concurrent unregistration and consumer shutdown completed safely." << std::endl;
	std::exit(EXIT_PASS);
}

static void test_concurrent_unregistration_blocking_probe() {
	std::cout << "=== Test 2: Concurrent Unregistration and Shutdown Probe ===" << std::endl;

	const char *broker_env = std::getenv("KAFKA_BROKERLIST");
	const std::string broker = broker_env ? broker_env : "";
	const std::string topic  = "test_concurrent_unregister_probe";

	std::cout << "  Broker: " << broker << "  Topic: " << topic << std::endl;
	std::cout << "  Forking child process; parent watchdog timeout: 65 s." << std::endl;

	pid_t child = fork();
	TEST_ASSERT(child >= 0, "fork() failed for Test 2");

	if (child == 0) {
		concurrent_child_body(topic);
	}

	// Parent watchdog: 65-second bounded wait using steady clock
	constexpr int timeout_seconds = 65;
	auto watchdog_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
	int status = 0;
	bool child_done = false;

	while (std::chrono::steady_clock::now() < watchdog_deadline) {
		pid_t ret = waitpid(child, &status, WNOHANG);
		if (ret == child) {
			child_done = true;
			break;
		}
		if (ret < 0) {
			std::cerr << "  ERROR: waitpid failed: " << strerror(errno) << std::endl;
			kill(child, SIGKILL);
			waitpid(child, nullptr, 0);
			std::exit(EXIT_FAIL);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
	}

	// Final check before declaring timeout
	if (!child_done) {
		pid_t final_check = waitpid(child, &status, WNOHANG);
		if (final_check == child) {
			child_done = true;
		}
	}

	if (!child_done) {
		kill(child, SIGKILL);
		waitpid(child, nullptr, 0);
		std::cerr << "  ERROR: Test 2 timed out after " << timeout_seconds
		          << " s and was killed by parent watchdog." << std::endl;
		std::exit(EXIT_FAIL);
	}

	if (WIFEXITED(status) && WEXITSTATUS(status) == EXIT_PASS) {
		return;
	} else if (WIFSIGNALED(status)) {
		std::cerr << "  ERROR: Test 2 child terminated by signal " << WTERMSIG(status) << std::endl;
		std::exit(EXIT_FAIL);
	} else {
		std::cerr << "  ERROR: Test 2 child exited with failure status " << WEXITSTATUS(status) << std::endl;
		std::exit(EXIT_FAIL);
	}
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
	Poco::AutoPtr<Poco::ConsoleChannel> pChannel(new Poco::ConsoleChannel);
	Poco::Logger::root().setChannel(pChannel);
	Poco::Logger::root().setLevel(Poco::Message::PRIO_INFORMATION);

	const char *broker = std::getenv("KAFKA_BROKERLIST");
	std::cout << "Running Kafka Callback Deadlock Diagnostic Probes..." << std::endl;
	std::cout << "(KAFKA_BROKERLIST="
	          << (broker ? broker : "<not set>")
	          << ")" << std::endl << std::endl;

	if (!broker || !*broker) {
		std::cout << "  SKIPPED: KAFKA_BROKERLIST not set." << std::endl;
		return EXIT_SKIP; // 77
	}

	test_reentrant_callback_self_deadlock_probe();
	std::cout << std::endl;
	test_concurrent_unregistration_blocking_probe();
	std::cout << std::endl;

	std::cout << "Kafka callback deadlock diagnostic probes completed; see observed results above." << std::endl;
	return EXIT_PASS;
}
