@echo off
setlocal
if "%QNX_SDP%"=="" set "QNX_SDP=C:\qnx800"
if exist "%QNX_SDP%\qnxsdp-env.bat" (
    call "%QNX_SDP%\qnxsdp-env.bat" >nul
)
pushd "%~dp0"
echo [Build] Compiling all 12 QNX Neutrino 8.0 native binaries with qcc -Vgcc_ntox86_64...

qcc -Vgcc_ntox86_64 -std=c11 -D_QNX_SOURCE -O2 -Wall -Wextra -Iinclude test_linter.c resmgr/ringbuffer.c linter/linter_engine.c -o test_linter_qnx -lm || goto :err
qcc -Vgcc_ntox86_64 -std=c11 -D_QNX_SOURCE -O2 -Wall -Wextra -Iinclude resmgr/safepulse_resmgr.c resmgr/ringbuffer.c linter/linter_engine.c linter/autosar_e2e.c resmgr/safe_state_fsm.c -o safepulse_resmgr -lm || goto :err
qcc -Vgcc_ntox86_64 -std=c11 -D_QNX_SOURCE -O2 -Wall -Wextra -Iinclude safepulse_watcher.c -o safepulse_watcher -lm || goto :err
qcc -Vgcc_ntox86_64 -std=c11 -D_QNX_SOURCE -O2 -Wall -Wextra -Iinclude test_resmgr_client.c resmgr/ringbuffer.c -o test_resmgr_client -lm || goto :err
qcc -Vgcc_ntox86_64 -std=c11 -D_QNX_SOURCE -O2 -Wall -Wextra -Iinclude safepulse_bridge.c resmgr/ringbuffer.c -o safepulse_bridge -lsocket -lm || goto :err
qcc -Vgcc_ntox86_64 -std=c11 -D_QNX_SOURCE -O2 -Wall -Wextra -Iinclude ecu/ecu_steering.c -o ecu/ecu_steering -lm || goto :err
qcc -Vgcc_ntox86_64 -std=c11 -D_QNX_SOURCE -O2 -Wall -Wextra -Iinclude ecu/ecu_braking.c -o ecu/ecu_braking -lm || goto :err
qcc -Vgcc_ntox86_64 -std=c11 -D_QNX_SOURCE -O2 -Wall -Wextra -Iinclude ecu/ecu_powertrain.c -o ecu/ecu_powertrain -lm || goto :err
qcc -Vgcc_ntox86_64 -std=c11 -D_QNX_SOURCE -O2 -Wall -Wextra -Iinclude test_stress_1000hz.c -o test_stress_1000hz -lm || goto :err
qcc -Vgcc_ntox86_64 -std=c11 -D_QNX_SOURCE -O2 -Wall -Wextra -Iinclude test_autosar_e2e.c linter/autosar_e2e.c resmgr/safe_state_fsm.c -o test_autosar_e2e -lm || goto :err
qcc -Vgcc_ntox86_64 -std=c11 -D_QNX_SOURCE -O2 -Wall -Wextra -Iinclude test_durability_1m.c -o test_durability_1m -lm || goto :err
qcc -Vgcc_ntox86_64 -std=c11 -D_QNX_SOURCE -O2 -Wall -Wextra -Iinclude benchmark_wcet.c resmgr/ringbuffer.c linter/linter_engine.c linter/autosar_e2e.c resmgr/safe_state_fsm.c -o benchmark_wcet -lm || goto :err
qcc -Vgcc_ntox86_64 -std=c11 -D_QNX_SOURCE -O2 -Wall -Wextra -Iinclude test_ffi_isolation.c resmgr/ringbuffer.c -o test_ffi_isolation -lm || goto :err

echo [Build] All 12 QNX 8.0 target binaries built successfully (0 errors).
popd
exit /b 0

:err
echo [Build] ERROR: Compilation failed!
popd
exit /b 1
