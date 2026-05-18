---
name: compilar
description: Compila el proyecto, corrige errores y hace commit si todo pasa
---

1. Ejecuta: platformio run --environment esp32dev
2. Si hay errores de compilación, corrígelos en src/main.cpp y vuelve a compilar
3. Si compila bien, ejecuta: git add . && git commit -m "fix: corregir errores de compilación"
