@echo off
setlocal
if "%QNX_SDP%"=="" set "QNX_SDP=C:\qnx800"
if exist "%QNX_SDP%\qnxsdp-env.bat" (
    call "%QNX_SDP%\qnxsdp-env.bat" >nul
)
pushd "%~dp0"
echo [Build] Compiling all 12 QNX Neutrino 8.0 native binaries with qcc -Vgcc_ntox86_64...

qcc -Vgcc_ntox86_64 -std=c11 -D_QNX_SOURCE -O2 -Wall -Wextra -Iinclude test_linter.c resmgr/ringbuffer.c linter/linter_engine.c linter/autosar_e2e.c -o test_linter_qnx -lm || goto :err
qcc -Vgcc_ntox86_64 -std=c11 -D_QNX_SOURCE -DSAFEPULSE_FAULT_INJECTION -O2 -Wall -Wextra -Iinclude resmgr/safepulse_resmgr.c resmgr/ringbuffer.c linter/linter_engine.c linter/autosar_e2e.c resmgr/safe_state_fsm.c -o safepulse_resmgr -lsecpol -lslog2 -lm || goto :err
qcc -Vgcc_ntox86_64 -std=c11 -D_QNX_SOURCE -O2 -Wall -Wextra -Iinclude safepulse_watcher.c -o safepulse_watcher -lsecpol -lslog2 -lm || goto :err
qcc -Vgcc_ntox86_64 -std=c11 -D_QNX_SOURCE -O2 -Wall -Wextra -Iinclude tools/rogue_writer.c linter/autosar_e2e.c resmgr/ringbuffer.c -o tools/rogue_writer -lm || goto :err
qcc -Vgcc_ntox86_64 -std=c11 -D_QNX_SOURCE -O2 -Wall -Wextra -Iinclude tools/read_stats.c -o tools/read_stats || goto :err
qcc -Vgcc_ntox86_64 -std=c11 -D_QNX_SOURCE -O2 -Wall -Wextra -Iinclude test_resmgr_client.c resmgr/ringbuffer.c linter/autosar_e2e.c -o test_resmgr_client -lm || goto :err
qcc -Vgcc_ntox86_64 -std=c11 -D_QNX_SOURCE -O2 -Wall -Wextra -Iinclude safepulse_bridge.c resmgr/ringbuffer.c linter/autosar_e2e.c -o safepulse_bridge -lsocket -lsecpol -lslog2 -lm || goto :err
qcc -Vgcc_ntox86_64 -std=c11 -D_QNX_SOURCE -O2 -Wall -Wextra -Iinclude ecu/ecu_steering.c linter/autosar_e2e.c -o ecu/ecu_steering -lm || goto :err
qcc -Vgcc_ntox86_64 -std=c11 -D_QNX_SOURCE -O2 -Wall -Wextra -Iinclude ecu/ecu_braking.c linter/autosar_e2e.c -o ecu/ecu_braking -lm || goto :err
qcc -Vgcc_ntox86_64 -std=c11 -D_QNX_SOURCE -O2 -Wall -Wextra -Iinclude ecu/ecu_powertrain.c linter/autosar_e2e.c -o ecu/ecu_powertrain -lm || goto :err
qcc -Vgcc_ntox86_64 -std=c11 -D_QNX_SOURCE -O2 -Wall -Wextra -Iinclude test_stress_1000hz.c -o test_stress_1000hz -lm || goto :err
qcc -Vgcc_ntox86_64 -std=c11 -D_QNX_SOURCE -O2 -Wall -Wextra -Iinclude test_autosar_e2e.c linter/autosar_e2e.c resmgr/safe_state_fsm.c resmgr/ringbuffer.c linter/linter_engine.c -o test_autosar_e2e -lm || goto :err
qcc -Vgcc_ntox86_64 -std=c11 -D_QNX_SOURCE -O2 -Wall -Wextra -Iinclude test_durability_1m.c -o test_durability_1m -lm || goto :err
qcc -Vgcc_ntox86_64 -std=c11 -D_QNX_SOURCE -O2 -Wall -Wextra -Iinclude benchmark_wcet.c resmgr/ringbuffer.c linter/linter_engine.c linter/autosar_e2e.c resmgr/safe_state_fsm.c -o benchmark_wcet -lm || goto :err
qcc -Vgcc_ntox86_64 -std=c11 -D_QNX_SOURCE -O2 -Wall -Wextra -Iinclude test_ffi_isolation.c resmgr/ringbuffer.c -o test_ffi_isolation -lm || goto :err

echo [Build] Archiving C SDK static library (libsafepulse.a)...
qcc -Vgcc_ntox86_64 -std=c11 -D_QNX_SOURCE -O2 -Wall -Wextra -I../include -Iinclude -c safepulse_api.c -o safepulse_api.o || goto :err
qcc -Vgcc_ntox86_64 -std=c11 -D_QNX_SOURCE -O2 -Wall -Wextra -Iinclude -c linter/linter_engine.c -o linter_engine.o || goto :err
qcc -Vgcc_ntox86_64 -std=c11 -D_QNX_SOURCE -O2 -Wall -Wextra -Iinclude -c linter/autosar_e2e.c -o autosar_e2e.o || goto :err
qcc -Vgcc_ntox86_64 -std=c11 -D_QNX_SOURCE -O2 -Wall -Wextra -Iinclude -c resmgr/safe_state_fsm.c -o safe_state_fsm.o || goto :err
qcc -Vgcc_ntox86_64 -std=c11 -D_QNX_SOURCE -O2 -Wall -Wextra -Iinclude -c resmgr/ringbuffer.c -o ringbuffer.o || goto :err
if exist libsafepulse.a del /Q libsafepulse.a
qcc -Vgcc_ntox86_64 -A libsafepulse.a safepulse_api.o linter_engine.o autosar_e2e.o safe_state_fsm.o ringbuffer.o || goto :err
del /Q safepulse_api.o linter_engine.o autosar_e2e.o safe_state_fsm.o ringbuffer.o
copy /Y libsafepulse.a ..\libsafepulse.a >nul || goto :err

echo [Build] Compiling C SDK verification test and 2 SDK examples...
qcc -Vgcc_ntox86_64 -std=c11 -D_QNX_SOURCE -O2 -Wall -Wextra -I../include -Iinclude test_sdk_rules.c ..\libsafepulse.a -o test_sdk_rules -lm || goto :err
qcc -Vgcc_ntox86_64 -std=c11 -D_QNX_SOURCE -O2 -Wall -Wextra -I../include -Iinclude ..\examples\minimal_c\main.c ..\libsafepulse.a -o ..\examples\minimal_c\example_minimal -lm || goto :err
qcc -Vgcc_ntox86_64 -std=c11 -D_QNX_SOURCE -O2 -Wall -Wextra -I../include -Iinclude ..\examples\custom_rules\main.c ..\libsafepulse.a -o ..\examples\custom_rules\example_custom_rules -lm || goto :err

echo [Build] All QNX 8.0 target binaries, libsafepulse.a, and SDK examples built successfully (0 errors).
popd
exit /b 0

:err
echo [Build] ERROR: Compilation failed!
popd
exit /b 1
