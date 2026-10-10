# GDB command file for debugging the IMU Attitude Lab C++ core.
#
# 1. Build with debug information:
#      cpp/scripts/build.sh Debug
#
# 2. Start GDB (add -tui for a source window):
#      gdb -x cpp/scripts/debug.gdb --args cpp/build/bin/imu_lab_cli stream --frames 500 --workers 3
#
# Windows/MinGW note: the bundled GDB tries to auto-load its Python helpers and
# prints a harmless traceback. To keep the console clean, start it with
#
#      env -u PYTHONPATH -u PYTHONHOME gdb -nx -x cpp/scripts/debug.gdb --args ...
#
# (Argument quoting on Git Bash/MINGW needs MSYS2_ARG_CONV_EXCL='*'; see
#  docs/CPP_MODULE.md for a ready-to-paste command.)

set pagination off
set confirm off
set print pretty on
set print elements 16
set print thread-events on
# Keep the other threads running while inspecting one: switching
# scheduler-locking to `on` is useful when hunting a data race.
set scheduler-locking off

# --------------------------------------------------------------------------- #
# Convenience commands
# --------------------------------------------------------------------------- #

#: Every thread with a two-frame backtrace: the fastest way to see which stage
#: a thread is parked in (queue wait, CSV write, source read...).
define threadsummary
    info threads
    echo \n--- per-thread backtrace (2 frames) ---\n
    thread apply all bt 2
end
document threadsummary
Print all threads plus a short backtrace for each.
end

#: Dump the whole pipeline state after the run has finished.
define showstats
    print run.stats
    printf "workers=%zu capacity=%zu highwater=%zu\n", run.stats.workers, run.stats.queue_capacity, run.stats.queue_high_water
    printf "received=%llu parsed=%llu rejected=%llu lost=%llu\n", run.stats.frames_received, run.stats.frames_parsed, run.stats.frames_rejected, run.stats.frames_received - run.stats.frames_parsed - run.stats.frames_rejected
end
document showstats
Print the consolidated PipelineStats struct and the integrity counters.
end

#: Trace mode: log every queue hand-off without stopping. Useful to confirm the
#: producer/consumer interleaving, noisy on large runs, so it is opt-in.
define trace-queue
    break 'imu_lab::ThreadSafeQueue<imu_lab::RawFrame>::push'
    commands
        silent
        printf "[producer %d] push seq=%llu\n", $_thread, value.sequence
        continue
    end
    break 'imu_lab::ThreadSafeQueue<imu_lab::RawFrame>::pop'
    commands
        silent
        printf "[worker   %d] pop\n", $_thread
        continue
    end
end
document trace-queue
Add auto-continuing breakpoints on both sides of the parse queue.
end

# --------------------------------------------------------------------------- #
# Default breakpoint: stops once, right where the run is being set up, so the
# session is interactive without stepping through a frame loop.
# --------------------------------------------------------------------------- #

break 'imu_lab::AttitudePipeline::run'

# Other useful stops (add them interactively, they are too noisy by default):
#   break 'imu_lab::AttitudePipeline::run' if csv_path.size() > 0
#   break imu_lab/src/pipeline.cpp:186        # after logger.join()
#   tbreak 'imu_lab::CsvReader::parse_row' if line_number > 3
#   break '__cxa_throw'                       # catch any escaping exception
#
# When stopped inside `run`, these read well:
#   print config_            -> workers, capacities, threshold
#   info threads             -> receiver / workers / logger
#   next, then info locals   -> queue sizes once they exist

echo \nGDB helpers loaded.\n
echo   threadsummary          -> all threads + short backtrace\n
echo   showstats              -> dump run.stats and the integrity counters\n
echo   trace-queue            -> log every queue hand-off without stopping\n
echo   break pipeline.cpp:186 -> stop at a specific line\n
echo \nStarting the program. Use `continue` to advance past each breakpoint.\n\n

run
