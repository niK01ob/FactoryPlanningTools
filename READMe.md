# FactoryPlanningTools

C++ проект для экспериментов по производственному планированию: сравнение baseline-эвристик и LLM-selector (опционально) на синтетических задачах.

## Что сейчас реализовано

- Парсинг задачи в `ProblemData` (включая `START/DIRECTIVE/FINES`).
- Solver с эвристиками сортировки фронта:
- `dummy`
- `directive` (минимальный директивный срок)
- `fine` (максимальный коэффициент штрафа)
- `round_robin` (циклический приоритет работ)
- `dependent` (операции, разблокирующие больше непосредственных потомков)
- `spt` (shortest processing time)
- `lpt` (longest processing time)
- `least_flexible` (операции с меньшим числом возможных инструментов)
- `slack_based` (операции работ с наименьшим оценочным запасом времени)
- Проверка корректности решения: `SolutionChecker`.
- Подсчёт штрафа: `Scorer`.
- Новый генератор задач: `GeneratorData` с preset-сложностью:
- быстрые smoke-пресеты: `small_easy|small_medium|small_hard`;
- крупные экспериментальные пресеты: `easy|medium|hard`.
- Batch-runner в `main` с выгрузкой:
- `baseline_results_long.csv`
- `baseline_results_wide.csv`
- `experiment_config.yaml`
- LLM-selector:
- `off` (выключен)
- `mock` (детерминированная имитация)
- `real` (вызов внешней LLM API)
- Логирование fallback в результатах:
- `llm_used_fallback`
- `llm_raw_response`

## Ключевые файлы

- `include/problem_data.h` — парсер и агрегатор задачи.
- `include/solver.h` — фронтальный алгоритм + эвристики.
- `include/generator_data.h` — новый генератор задач.
- `include/task_profile.h` — извлечение признаков и `TASK_PROFILE_V1`.
- `include/llm_selector.h` — selector (`off/mock/real`) + fallback.
- `src/main.cpp` — batch-прогоны и сохранение артефактов.
- `scripts/summarize_results.py` — сводные таблицы `LLM vs baselines`.

## Сборка

```bash
cmake -S . -B build-gcc -G Ninja \
  -DCMAKE_C_COMPILER="<path-to-gcc>" \
  -DCMAKE_CXX_COMPILER="<path-to-g++>"

cmake --build build-gcc --target FactoryPlanningTools -j 4
cmake --build build-gcc --target FactoryPlanningTools.tests -j 4
```

## Быстрый запуск экспериментов

### Baselines без LLM

```bash
./build-gcc/FactoryPlanningTools.exe --tasks=200 --difficulty=small_medium
```

Для крупных дипломных экспериментов используйте новые большие preset-ы:

```bash
./build-gcc/FactoryPlanningTools.exe --tasks=200 --difficulty=medium
```

### С LLM mock

```bash
./build-gcc/FactoryPlanningTools.exe --tasks=200 --difficulty=small_medium --llm=mock
```

### Только генерация корпуса задач

Этот режим не запускает solver и LLM. Он нужен, чтобы зафиксировать один набор
задач для честного сравнения разных режимов: `no llm`, base LLM,
fine-tuned LLM, а также для подготовки prompt-датасета.

```bash
./build-gcc/FactoryPlanningTools.exe \
  --generate-only \
  --tasks=10000 \
  --difficulty=medium \
  --base-seed=123456 \
  --out-dir=generated_medium_10k
```

Выходная структура:
- `tasks/*.task` — задачи в текстовом формате `TASK_DATA_V1`.
- `profiles/*.txt` — `TASK_PROFILE_V1` для каждой задачи.
- `prompts/*.json` — OpenAI-style messages для каждой задачи.
- `prompts.jsonl` — все prompt-ы в JSONL, удобно для дальнейшей разметки.
- `task_profiles.csv` — табличные признаки задач.
- `dataset_config.yaml` — параметры генерации и seed.

Важно: `--base-seed` фиксирует корпус. При одинаковых `--tasks`,
`--difficulty` и `--base-seed` генератор создаёт тот же набор задач.

### Запуск сохранённых задач `TASK_DATA_V1`

Один файл:

```bash
./build-gcc/FactoryPlanningTools.exe \
  --task-file=generated_medium_10k/tasks/task_000000.task \
  --base-seed=777
```

Папка с `.task` файлами:

```bash
./build-gcc/FactoryPlanningTools.exe \
  --tasks-dir=generated_medium_10k/tasks \
  --base-seed=777
```

Важно: в `--tasks-dir` передаётся именно папка, где напрямую лежат `.task`
файлы. Для корпуса с разными пресетами запускайте подпапки отдельно, например:

```bash
./build-gcc/FactoryPlanningTools.exe --tasks-dir=diploma_dataset_8000/easy/tasks --base-seed=777
./build-gcc/FactoryPlanningTools.exe --tasks-dir=diploma_dataset_8000/medium/tasks --base-seed=777
./build-gcc/FactoryPlanningTools.exe --tasks-dir=diploma_dataset_8000/hard/tasks --base-seed=777
```

Если нужно быстро проверить часть папки, добавьте `--tasks=N`: будут взяты
первые `N` файлов после сортировки по имени.

### С real LLM

```bash
# Linux/macOS
export LLM_SELECTOR_MODE=real
export LLM_API_KEY=<your_key>
export LLM_MODEL=gpt-4o-mini
export LLM_TIMEOUT_MS=20000
export LLM_ENDPOINT=https://api.openai.com/v1/chat/completions

./build-gcc/FactoryPlanningTools.exe --tasks=200 --difficulty=small_medium --llm=real
```

```powershell
# Windows PowerShell
$env:LLM_SELECTOR_MODE="real"
$env:LLM_API_KEY="<your_key>"
$env:LLM_MODEL="gpt-4o-mini"
$env:LLM_TIMEOUT_MS="20000"
$env:LLM_ENDPOINT="https://api.openai.com/v1/chat/completions"

.\build-gcc\FactoryPlanningTools.exe --tasks=200 --difficulty=small_medium --llm=real
```

### С real LLM через SSH-туннель

`LLMSelector` отправляет запрос в OpenAI-compatible Chat Completions API:

- метод: `POST`
- endpoint: `LLM_ENDPOINT`
- формат: `/v1/chat/completions`
- обязательный заголовок: `Authorization: Bearer <LLM_API_KEY>`
- ожидаемый ответ: `choices[0].message.content` с одним токеном эвристики, например `DIRECTIVE`

Если LLM запущена на удаленной машине, а локально открыт SSH-туннель, указывайте локальный адрес туннеля. Например, если туннель пробрасывает удаленный порт `8000` на локальный `8000`:

```powershell
$env:LLM_SELECTOR_MODE="real"
$env:LLM_ENDPOINT="http://127.0.0.1:8000/v1/chat/completions"
$env:LLM_MODEL="<served-model-name>"
$env:LLM_TIMEOUT_MS="30000"

# Если сервер не проверяет авторизацию, все равно задайте любое непустое значение:
$env:LLM_API_KEY="local"

# Если сервер требует токен, задайте реальный токен вместо local:
# $env:LLM_API_KEY="<server-token>"

.\build-gcc\FactoryPlanningTools.exe --tasks=5 --difficulty=small_easy --llm=real --method-threads=9
```

Аналогично для Linux/macOS:

```bash
export LLM_SELECTOR_MODE=real
export LLM_ENDPOINT=http://127.0.0.1:8000/v1/chat/completions
export LLM_MODEL=<served-model-name>
export LLM_TIMEOUT_MS=30000
export LLM_API_KEY=local

./build-gcc/FactoryPlanningTools.exe --tasks=5 --difficulty=small_easy --llm=real --method-threads=9
```

Важно: `LLM_API_KEY` сейчас должен быть непустым даже для локального туннеля. Иначе real-режим не делает HTTP-запрос и пишет в CSV `REAL_MODE_NO_API_KEY`.

Проверенный пример для Ollama/OpenAI-compatible endpoint на локальном порту `11434`:

```powershell
$env:PATH="$env:USERPROFILE\scoop\apps\gcc\current\bin;$env:PATH"
$env:LLM_SELECTOR_MODE="real"
$env:LLM_API_KEY="ollama"
$env:LLM_MODEL="qwen2.5:3b"
$env:LLM_TIMEOUT_MS="60000"
$env:LLM_ENDPOINT="http://127.0.0.1:11434/v1/chat/completions"

.\FactoryPlanningTools.exe --tasks=30 --difficulty=small_medium --llm=real
```

Для этого прогона ожидаемый признак успешной работы real-режима: в `baseline_results_long.csv` у строк `method=llm` поле `llm_used_fallback` равно `0`.

## Артефакты прогона

После запуска в `build-gcc/` создаются:

- `baseline_results_long.csv`
- `baseline_results_wide.csv`
- `experiment_config.yaml`

`long` CSV колонки:
- `task_id,seed,method,valid,score,runtime_ms,selected_heuristic,llm_latency_ms,llm_used_fallback,llm_raw_response`

`wide` CSV:
- 1 строка = 1 задача
- отдельные столбцы по каждому методу (`*_valid`, `*_score`, `*_runtime_ms`, `*_selected_heuristic`, `*_llm_latency_ms`, `*_llm_used_fallback`)

## Сводные таблицы LLM vs baselines

```bash
python scripts/summarize_results.py \
  --input build-gcc/baseline_results_long.csv \
  --target llm \
  --out-dir build-gcc/summary
```

Выход:
- `method_summary.csv`
- `pairwise_vs_target.csv`
- `summary_report.md`
- `summary_report.html` (с фильтрацией и сортировкой колонок)

## Настройка конкретной LLM-модели (отдельный чеклист)

1. Выберите модель и endpoint.
- По умолчанию endpoint: `https://api.openai.com/v1/chat/completions`.
- Модель задаётся через `LLM_MODEL`.
- Для SSH-туннеля обычно нужен endpoint вида `http://127.0.0.1:<local_port>/v1/chat/completions`.
- Сервер должен быть совместим с OpenAI Chat Completions API.

2. Установите переменные окружения.
- `LLM_SELECTOR_MODE=real`
- `LLM_API_KEY=<ключ>`; для локального сервера без авторизации можно указать любое непустое значение, например `local`.
- `LLM_MODEL=<имя модели>`
- `LLM_TIMEOUT_MS=<таймаут_мс>`
- опционально `LLM_ENDPOINT=<url>`

3. Запустите короткий smoke-прогон.
- `--tasks=10 --llm=real`

4. Проверьте fallback-логи в `baseline_results_long.csv`.
- `method=llm`
- `llm_used_fallback=0` ожидается при корректном ответе модели.
- если `llm_used_fallback=1`, смотрите `llm_raw_response` (там причина/сырой ответ).

5. Зафиксируйте конфиг эксперимента.
- Используйте `experiment_config.yaml` как артефакт воспроизводимости (`llm_mode`, `llm_model`, `llm_timeout_ms`, `llm_endpoint`, `base_seed`).

## Важно про fallback

Fallback включён всегда для отказоустойчивости:
- при невалидном ответе LLM,
- при ошибке вызова API,
- при пустом ответе,

В текущей реализации fallback-эвристика по умолчанию — `DIRECTIVE`
(`LLMSelector::Config::fallback`). Это явно отражается в логах:
- `llm_used_fallback=1`
- `llm_raw_response=<текст ошибки/ответа>`
