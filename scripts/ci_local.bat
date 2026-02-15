@echo off
REM =============================================================================
REM MirageSystem Local CI Script
REM Run from project root: scripts\ci_local.bat
REM =============================================================================
setlocal enabledelayedexpansion

echo ============================================
echo  MirageSystem Local CI
echo  %date% %time%
echo ============================================
echo.

set FAIL=0
set BUILD_DIR=build

REM --- Step 1: CMake configure ---
echo [1/5] CMake configure...
cd /d %~dp0\..
if not exist %BUILD_DIR% mkdir %BUILD_DIR%
cd %BUILD_DIR%
cmake .. -G "MinGW Makefiles" >nul 2>&1
if %errorlevel% neq 0 (
    echo  FAIL: CMake configure
    set FAIL=1
    goto :summary
)
echo  OK

REM --- Step 2: Build main ---
echo [2/5] Build mirage_gui...
mingw32-make -j4 mirage_gui 2>build_errors.txt
if %errorlevel% neq 0 (
    echo  FAIL: Build
    type build_errors.txt
    set FAIL=1
    goto :summary
)
echo  OK

REM --- Step 3: Build tests ---
echo [3/5] Build tests...
mingw32-make -j4 mirage_tests test_aoa_hid_touch test_device_registry 2>test_build_errors.txt
if %errorlevel% neq 0 (
    echo  FAIL: Test build
    type test_build_errors.txt
    set FAIL=1
    goto :summary
)
echo  OK

REM --- Step 4: Run gtest ---
echo [4/5] Run gtest (104 tests)...
mirage_tests.exe >test_gtest_out.txt 2>nul
findstr /C:"PASSED" test_gtest_out.txt >nul 2>&1
if %errorlevel% neq 0 (
    echo  FAIL: gtest
    type test_gtest_out.txt
    set FAIL=1
) else (
    for /f "tokens=*" %%a in ('findstr /C:"PASSED" test_gtest_out.txt') do echo  %%a
)

REM --- Step 5: Run standalone tests ---
echo [5/5] Run standalone tests...
test_aoa_hid_touch.exe >test_hid_out.txt 2>nul
findstr /C:"ALL 8 TESTS PASSED" test_hid_out.txt >nul 2>&1
if %errorlevel% neq 0 (
    echo  FAIL: HID tests
    set FAIL=1
) else (
    echo  HID: 8/8 PASS
)

test_device_registry.exe >test_reg_out.txt 2>nul
findstr /C:"ALL TESTS PASSED" test_reg_out.txt >nul 2>&1
if %errorlevel% neq 0 (
    echo  FAIL: Registry tests
    set FAIL=1
) else (
    echo  Registry: 12/12 PASS
)

:summary
echo.
echo ============================================
if %FAIL% equ 0 (
    echo  ALL CHECKS PASSED
) else (
    echo  SOME CHECKS FAILED
)
echo ============================================

cd /d %~dp0\..
exit /b %FAIL%
