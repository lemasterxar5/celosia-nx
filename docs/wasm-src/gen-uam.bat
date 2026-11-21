@echo off
rem Regenerates uam-gen\ from devkitPro uam 1.1.0's grammars (C:\Users\emi\uam-1.1.0), as its meson.build does.
setlocal
set U=C:\Users\emi\uam-1.1.0\mesa-imported\glsl
set O=%~dp0uam-gen\glsl
set W=C:\Users\emi\winflexbison
set PY=C:\Users\emi\uam-gen-venv\Scripts\python.exe
mkdir "%O%\glcpp" 2>nul
cd /d "%U%"
"%W%\win_bison.exe" -o "%O%\glsl_parser.cpp" -p _mesa_glsl_ --defines="%O%\glsl_parser.h" glsl_parser.yy || exit /b 1
"%W%\win_flex.exe" -o "%O%\glsl_lexer.cpp" glsl_lexer.ll || exit /b 1
"%PY%" ir_expression_operation.py enum > "%O%\ir_expression_operation.h" || exit /b 1
"%PY%" ir_expression_operation.py constant > "%O%\ir_expression_operation_constant.h" || exit /b 1
"%PY%" ir_expression_operation.py strings > "%O%\ir_expression_operation_strings.h" || exit /b 1
cd glcpp
"%W%\win_bison.exe" -o "%O%\glcpp\glcpp-parse.c" -p glcpp_parser_ --defines="%O%\glcpp\glcpp-parse.h" glcpp-parse.y || exit /b 1
"%W%\win_flex.exe" -o "%O%\glcpp\glcpp-lex.c" glcpp-lex.l || exit /b 1
