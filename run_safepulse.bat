@echo off
pushd "%~dp0"
echo ==================================================
echo  Starting QNX SafePulse Lab (QEMU + Web Bridge)
echo ==================================================

python "%~dp0run_safepulse.py" %*
popd
