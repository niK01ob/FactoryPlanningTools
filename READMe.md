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
- Новый генератор задач: `GeneratorDataV2` с preset-сложностью `easy|medium|hard`.
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
- `include/generator_data_v2.h` — новый генератор задач.
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
./build-gcc/FactoryPlanningTools.exe --tasks=200 --difficulty=medium
```

### С LLM mock

```bash
./build-gcc/FactoryPlanningTools.exe --tasks=200 --difficulty=medium --llm=mock
```

### С real LLM

```bash
# Linux/macOS
export LLM_SELECTOR_MODE=real
export LLM_API_KEY=<your_key>
export LLM_MODEL=gpt-4o-mini
export LLM_TIMEOUT_MS=20000

./build-gcc/FactoryPlanningTools.exe --tasks=200 --difficulty=medium --llm=real
```

```powershell
# Windows PowerShell
$env:LLM_SELECTOR_MODE="real"
$env:LLM_API_KEY="<your_key>"
$env:LLM_MODEL="gpt-4o-mini"
$env:LLM_TIMEOUT_MS="20000"

.\build-gcc\FactoryPlanningTools.exe --tasks=200 --difficulty=medium --llm=real
```

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

2. Установите переменные окружения.
- `LLM_SELECTOR_MODE=real`
- `LLM_API_KEY=<ключ>`
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

selector автоматически выбирает `DIRECTIVE` и это явно отражается в логах:
- `llm_used_fallback=1`
- `llm_raw_response=<текст ошибки/ответа>`
