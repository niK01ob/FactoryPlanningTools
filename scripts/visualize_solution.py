#!/usr/bin/env python3
import argparse
import json
import math
from pathlib import Path
import tkinter as tk
from tkinter import filedialog, messagebox, ttk
from typing import Optional


PALETTE = [
    "#4E79A7",
    "#F28E2B",
    "#E15759",
    "#76B7B2",
    "#59A14F",
    "#EDC948",
    "#B07AA1",
    "#FF9DA7",
    "#9C755F",
    "#BAB0AC",
    "#2F4B7C",
    "#A05195",
    "#D45087",
    "#F95D6A",
    "#FF7C43",
    "#00A6A6",
]


def load_solution(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as f:
        data = json.load(f)
    if data.get("format") != "SOLUTION_VISUALIZATION_V1":
        raise ValueError("Файл не похож на SOLUTION_VISUALIZATION_V1")
    for key in ("tools", "works", "operations", "assignments"):
        if key not in data or not isinstance(data[key], list):
            raise ValueError(f"В JSON нет обязательного массива: {key}")
    return data


def as_int(value, default=-1):
    try:
        if value is None:
            return default
        return int(value)
    except Exception:
        return default


class ScheduleVisualizer(tk.Tk):
    def __init__(self, initial_file: Optional[Path] = None):
        super().__init__()
        self.title("FactoryPlanningTools: визуализация решения")
        self.geometry("1280x820")
        self.minsize(980, 620)

        self.data: Optional[dict] = None
        self.path: Optional[Path] = None
        self.work_by_id = {}
        self.operation_by_id = {}
        self.assignments = []
        self.selected_assignment = None
        self.selected_work = tk.StringVar(value="Все работы")
        self.selected_tool = tk.StringVar(value="Все исполнители")
        self.zoom = tk.DoubleVar(value=0.10)
        self.show_schedule = tk.BooleanVar(value=True)
        self.status = tk.StringVar(value="Откройте JSON-файл решения")

        self._build_ui()
        self._bind_events()

        if initial_file:
            self.open_path(initial_file)

    def _build_ui(self):
        root = ttk.Frame(self, padding=8)
        root.pack(fill=tk.BOTH, expand=True)

        toolbar = ttk.Frame(root)
        toolbar.pack(fill=tk.X)

        ttk.Button(toolbar, text="Открыть JSON", command=self.open_dialog).pack(
            side=tk.LEFT
        )
        ttk.Separator(toolbar, orient=tk.VERTICAL).pack(
            side=tk.LEFT, fill=tk.Y, padx=8
        )

        ttk.Label(toolbar, text="Работа").pack(side=tk.LEFT)
        self.work_combo = ttk.Combobox(
            toolbar,
            textvariable=self.selected_work,
            state="readonly",
            width=22,
            values=["Все работы"],
        )
        self.work_combo.pack(side=tk.LEFT, padx=(4, 12))

        ttk.Label(toolbar, text="Исполнитель").pack(side=tk.LEFT)
        self.tool_combo = ttk.Combobox(
            toolbar,
            textvariable=self.selected_tool,
            state="readonly",
            width=22,
            values=["Все исполнители"],
        )
        self.tool_combo.pack(side=tk.LEFT, padx=(4, 12))

        ttk.Checkbutton(
            toolbar,
            text="Окна доступности",
            variable=self.show_schedule,
            command=self.redraw_all,
        ).pack(side=tk.LEFT, padx=(0, 12))

        ttk.Label(toolbar, text="Масштаб").pack(side=tk.LEFT)
        ttk.Scale(
            toolbar,
            from_=0.02,
            to=0.50,
            variable=self.zoom,
            orient=tk.HORIZONTAL,
            command=lambda _value: self.redraw_gantt(),
            length=180,
        ).pack(side=tk.LEFT, padx=(4, 12))

        self.summary_label = ttk.Label(toolbar, text="")
        self.summary_label.pack(side=tk.RIGHT)

        self.main_pane = ttk.PanedWindow(root, orient=tk.HORIZONTAL)
        self.main_pane.pack(fill=tk.BOTH, expand=True, pady=(8, 6))

        self.left = ttk.Frame(self.main_pane, width=330)
        self.right = ttk.Frame(self.main_pane)
        self.main_pane.add(self.left, weight=0)
        self.main_pane.add(self.right, weight=1)

        self._build_left_panel()
        self._build_tabs()

        status = ttk.Label(root, textvariable=self.status, anchor=tk.W)
        status.pack(fill=tk.X)

    def _build_left_panel(self):
        ttk.Label(self.left, text="Работы").pack(anchor=tk.W)
        work_cols = ("start", "directive", "completion", "penalty")
        self.work_tree = ttk.Treeview(
            self.left,
            columns=work_cols,
            show="tree headings",
            height=12,
            selectmode="browse",
        )
        self.work_tree.heading("#0", text="ID")
        self.work_tree.column("#0", width=58, stretch=False)
        for col, text, width in (
            ("start", "Старт", 58),
            ("directive", "Срок", 58),
            ("completion", "Финиш", 58),
            ("penalty", "Штраф", 70),
        ):
            self.work_tree.heading(col, text=text)
            self.work_tree.column(col, width=width, anchor=tk.E, stretch=False)
        self.work_tree.pack(fill=tk.BOTH, expand=True, pady=(4, 8))

        ttk.Label(self.left, text="Детали").pack(anchor=tk.W)
        self.detail_text = tk.Text(
            self.left,
            height=12,
            wrap=tk.WORD,
            borderwidth=1,
            relief=tk.SOLID,
            font=("Consolas", 10),
        )
        self.detail_text.pack(fill=tk.BOTH, expand=True, pady=(4, 8))
        self.detail_text.configure(state=tk.DISABLED)

        ttk.Label(self.left, text="Операции выбранной работы").pack(anchor=tk.W)
        op_cols = ("start", "end", "tools", "parents")
        self.op_tree = ttk.Treeview(
            self.left,
            columns=op_cols,
            show="tree headings",
            height=9,
            selectmode="browse",
        )
        self.op_tree.heading("#0", text="Опер.")
        self.op_tree.column("#0", width=55, stretch=False)
        for col, text, width in (
            ("start", "Старт", 58),
            ("end", "Конец", 58),
            ("tools", "Станки", 70),
            ("parents", "Пред.", 70),
        ):
            self.op_tree.heading(col, text=text)
            self.op_tree.column(col, width=width, anchor=tk.E, stretch=False)
        self.op_tree.pack(fill=tk.BOTH, expand=True, pady=(4, 0))

    def _build_tabs(self):
        self.tabs = ttk.Notebook(self.right)
        self.tabs.pack(fill=tk.BOTH, expand=True)

        self.gantt_tab = ttk.Frame(self.tabs)
        self.util_tab = ttk.Frame(self.tabs)
        self.info_tab = ttk.Frame(self.tabs)
        self.tabs.add(self.gantt_tab, text="Гант")
        self.tabs.add(self.util_tab, text="Загрузка")
        self.tabs.add(self.info_tab, text="Сводка")

        gantt_frame = ttk.Frame(self.gantt_tab)
        gantt_frame.pack(fill=tk.BOTH, expand=True)
        self.gantt_canvas = tk.Canvas(gantt_frame, bg="white", highlightthickness=0)
        self.gantt_vbar = ttk.Scrollbar(
            gantt_frame, orient=tk.VERTICAL, command=self.gantt_canvas.yview
        )
        self.gantt_hbar = ttk.Scrollbar(
            gantt_frame, orient=tk.HORIZONTAL, command=self.gantt_canvas.xview
        )
        self.gantt_canvas.configure(
            yscrollcommand=self.gantt_vbar.set,
            xscrollcommand=self.gantt_hbar.set,
        )
        self.gantt_canvas.grid(row=0, column=0, sticky="nsew")
        self.gantt_vbar.grid(row=0, column=1, sticky="ns")
        self.gantt_hbar.grid(row=1, column=0, sticky="ew")
        gantt_frame.rowconfigure(0, weight=1)
        gantt_frame.columnconfigure(0, weight=1)

        util_frame = ttk.Frame(self.util_tab)
        util_frame.pack(fill=tk.BOTH, expand=True)
        self.util_canvas = tk.Canvas(util_frame, bg="white", highlightthickness=0)
        self.util_vbar = ttk.Scrollbar(
            util_frame, orient=tk.VERTICAL, command=self.util_canvas.yview
        )
        self.util_canvas.configure(yscrollcommand=self.util_vbar.set)
        self.util_canvas.grid(row=0, column=0, sticky="nsew")
        self.util_vbar.grid(row=0, column=1, sticky="ns")
        util_frame.rowconfigure(0, weight=1)
        util_frame.columnconfigure(0, weight=1)

        self.summary_text = tk.Text(
            self.info_tab,
            wrap=tk.WORD,
            borderwidth=0,
            font=("Consolas", 10),
        )
        self.summary_text.pack(fill=tk.BOTH, expand=True)
        self.summary_text.configure(state=tk.DISABLED)

    def _bind_events(self):
        self.work_combo.bind("<<ComboboxSelected>>", self._on_work_combo)
        self.tool_combo.bind("<<ComboboxSelected>>", lambda _event: self.redraw_all())
        self.work_tree.bind("<<TreeviewSelect>>", self._on_work_tree_select)
        self.op_tree.bind("<<TreeviewSelect>>", self._on_op_tree_select)
        self.gantt_canvas.bind("<Button-1>", self._on_gantt_click)
        self.gantt_canvas.bind("<MouseWheel>", self._on_canvas_wheel)
        self.util_canvas.bind("<MouseWheel>", self._on_canvas_wheel)

    def open_dialog(self):
        initial_dir = self.path.parent if self.path else Path.cwd()
        file_name = filedialog.askopenfilename(
            title="Выберите JSON решения",
            initialdir=str(initial_dir),
            filetypes=(("Solution JSON", "*.json"), ("All files", "*.*")),
        )
        if file_name:
            self.open_path(Path(file_name))

    def open_path(self, path: Path):
        try:
            self.data = load_solution(path)
        except Exception as exc:
            messagebox.showerror("Не удалось открыть файл", str(exc))
            return

        self.path = path
        self.work_by_id = {
            as_int(work.get("work_id")): work for work in self.data["works"]
        }
        self.operation_by_id = {
            as_int(op.get("operation_id")): op for op in self.data["operations"]
        }
        self.assignments = sorted(
            self.data["assignments"],
            key=lambda a: (
                as_int(a.get("tool_id")),
                as_int(a.get("start")),
                as_int(a.get("operation_id")),
            ),
        )
        self.selected_assignment = None
        self._refresh_controls()
        self._refresh_work_tree()
        self._refresh_summary()
        self.redraw_all()
        self.status.set(f"Открыт файл: {path}")

    def _refresh_controls(self):
        assert self.data is not None
        work_values = ["Все работы"] + [
            f"Работа {as_int(w.get('work_id'))}" for w in self.data["works"]
        ]
        tool_values = ["Все исполнители"] + [
            f"Исполнитель {as_int(t.get('tool_id'))}" for t in self.data["tools"]
        ]
        self.work_combo.configure(values=work_values)
        self.tool_combo.configure(values=tool_values)
        self.selected_work.set("Все работы")
        self.selected_tool.set("Все исполнители")

        self.summary_label.configure(
            text=(
                f"task={self.data.get('task_id')}  "
                f"method={self.data.get('method')}  "
                f"score={self.data.get('score')}"
            )
        )

    def _refresh_work_tree(self):
        self.work_tree.delete(*self.work_tree.get_children())
        if not self.data:
            return
        for work in self.data["works"]:
            work_id = as_int(work.get("work_id"))
            self.work_tree.insert(
                "",
                tk.END,
                iid=str(work_id),
                text=str(work_id),
                values=(
                    work.get("start", ""),
                    work.get("directive", ""),
                    work.get("completion", ""),
                    f"{float(work.get('penalty', 0.0)):.2f}",
                ),
            )

    def _refresh_summary(self):
        if not self.data:
            return
        lines = [
            f"Файл: {self.path}",
            f"Формат: {self.data.get('format')}",
            f"Задача: {self.data.get('task_id')}",
            f"Seed: {self.data.get('seed')}",
            f"Метод: {self.data.get('method')}",
            f"Выбранная эвристика: {self.data.get('selected_heuristic')}",
            f"Корректное решение: {self.data.get('valid')}",
            f"Score: {self.data.get('score')}",
            f"Runtime, ms: {self.data.get('runtime_ms')}",
            "",
            f"Исполнителей: {len(self.data['tools'])}",
            f"Работ: {len(self.data['works'])}",
            f"Операций: {len(self.data['operations'])}",
            f"Интервалов назначений: {len(self.data['assignments'])}",
            "",
            "Работы с ненулевым опозданием:",
        ]
        late = [
            w
            for w in self.data["works"]
            if float(w.get("tardiness", 0) or 0) > 0
        ]
        if late:
            for work in late:
                lines.append(
                    "  "
                    f"work={work.get('work_id')} "
                    f"tardiness={work.get('tardiness')} "
                    f"penalty={work.get('penalty')}"
                )
        else:
            lines.append("  нет")
        self._set_text(self.summary_text, "\n".join(lines))

    def redraw_all(self):
        self.redraw_gantt()
        self.redraw_utilization()
        self._refresh_operation_tree()
        self._refresh_detail_text()

    def redraw_gantt(self):
        canvas = self.gantt_canvas
        canvas.delete("all")
        if not self.data:
            canvas.create_text(24, 24, anchor=tk.NW, text="Откройте JSON-файл")
            return

        work_filter = self._current_work_id()
        tool_filter = self._current_tool_id()
        tools = [
            t
            for t in self.data["tools"]
            if tool_filter is None or as_int(t.get("tool_id")) == tool_filter
        ]
        assignments = [
            a
            for a in self.assignments
            if (work_filter is None or as_int(a.get("work_id")) == work_filter)
            and (tool_filter is None or as_int(a.get("tool_id")) == tool_filter)
        ]

        max_time = self._max_time(tools, assignments)
        scale = self.zoom.get()
        left = 116
        top = 48
        row_h = 42
        chart_w = max(900, int(max_time * scale) + 180)
        chart_h = max(480, top + len(tools) * row_h + 80)

        self._draw_time_axis(canvas, left, top, chart_w, max_time, scale)

        assignment_tags = {}
        for row, tool in enumerate(tools):
            tool_id = as_int(tool.get("tool_id"))
            y = top + row * row_h
            canvas.create_text(
                left - 12,
                y + row_h / 2,
                anchor=tk.E,
                text=f"Исп. {tool_id}",
                fill="#243447",
            )
            canvas.create_line(left, y + row_h, chart_w, y + row_h, fill="#ECEFF3")

            if self.show_schedule.get():
                for start, end in tool.get("schedule", []):
                    x1 = left + start * scale
                    x2 = left + end * scale
                    canvas.create_rectangle(
                        x1,
                        y + 8,
                        x2,
                        y + row_h - 8,
                        fill="#F2F5F8",
                        outline="",
                    )

            for assignment in assignments:
                if as_int(assignment.get("tool_id")) != tool_id:
                    continue
                start = as_int(assignment.get("start"), 0)
                end = as_int(assignment.get("end"), 0)
                op_id = as_int(assignment.get("operation_id"))
                work_id = as_int(assignment.get("work_id"))
                x1 = left + start * scale
                x2 = max(x1 + 2, left + end * scale)
                color = self._work_color(work_id)
                tag = f"assign_{len(assignment_tags)}"
                assignment_tags[tag] = assignment
                canvas.create_rectangle(
                    x1,
                    y + 11,
                    x2,
                    y + row_h - 11,
                    fill=color,
                    outline="#1C2630",
                    width=1,
                    tags=(tag, "assignment"),
                )
                if x2 - x1 >= 34:
                    canvas.create_text(
                        x1 + 4,
                        y + row_h / 2,
                        anchor=tk.W,
                        text=f"o{op_id}",
                        fill="white",
                        font=("Segoe UI", 8, "bold"),
                        tags=(tag, "assignment"),
                    )

        canvas.assignment_tags = assignment_tags
        if work_filter is not None:
            self._draw_work_deadlines(canvas, left, top, chart_h, scale, work_filter)
        canvas.configure(scrollregion=(0, 0, chart_w, chart_h))

    def redraw_utilization(self):
        canvas = self.util_canvas
        canvas.delete("all")
        if not self.data:
            canvas.create_text(24, 24, anchor=tk.NW, text="Откройте JSON-файл")
            return

        tool_filter = self._current_tool_id()
        tools = [
            t
            for t in self.data["tools"]
            if tool_filter is None or as_int(t.get("tool_id")) == tool_filter
        ]
        left = 128
        top = 42
        row_h = 34
        bar_w = 560
        height = max(420, top + len(tools) * row_h + 64)
        width = 820
        canvas.create_text(
            left,
            16,
            anchor=tk.W,
            text="Загруженность исполнителей",
            font=("Segoe UI", 12, "bold"),
            fill="#243447",
        )
        for row, tool in enumerate(tools):
            tool_id = as_int(tool.get("tool_id"))
            available = float(tool.get("available_time", 0) or 0)
            busy = float(tool.get("busy_time", 0) or 0)
            util = float(tool.get("utilization", 0.0) or 0.0)
            y = top + row * row_h
            canvas.create_text(left - 12, y + 12, anchor=tk.E, text=f"Исп. {tool_id}")
            canvas.create_rectangle(left, y, left + bar_w, y + 22, fill="#F2F5F8", outline="")
            fill_w = max(0, min(bar_w, bar_w * util))
            canvas.create_rectangle(
                left,
                y,
                left + fill_w,
                y + 22,
                fill="#4E79A7",
                outline="",
            )
            canvas.create_text(
                left + bar_w + 12,
                y + 11,
                anchor=tk.W,
                text=f"{util * 100:.1f}%   {busy:.0f}/{available:.0f}",
                fill="#243447",
            )
        canvas.configure(scrollregion=(0, 0, width, height))

    def _draw_time_axis(self, canvas, left, top, width, max_time, scale):
        canvas.create_line(left, top - 12, width, top - 12, fill="#8A94A6")
        step = self._nice_tick_step(max_time)
        tick = 0
        while tick <= max_time + step:
            x = left + tick * scale
            if x > width:
                break
            canvas.create_line(x, top - 18, x, top - 6, fill="#8A94A6")
            canvas.create_text(x, top - 24, anchor=tk.S, text=str(tick), fill="#5B6472")
            canvas.create_line(x, top, x, top + 5000, fill="#F0F2F5")
            tick += step

    def _draw_work_deadlines(self, canvas, left, top, chart_h, scale, work_id):
        work = self.work_by_id.get(work_id)
        if not work:
            return
        for key, color, label in (
            ("start", "#2E7D32", "start"),
            ("directive", "#C62828", "due"),
            ("completion", "#455A64", "finish"),
        ):
            value = as_int(work.get(key), -1)
            if value < 0:
                continue
            x = left + value * scale
            canvas.create_line(x, top - 10, x, chart_h - 30, fill=color, dash=(4, 3))
            canvas.create_text(x + 4, top + 4, anchor=tk.NW, text=label, fill=color)

    def _refresh_operation_tree(self):
        self.op_tree.delete(*self.op_tree.get_children())
        if not self.data:
            return
        work_id = self._current_work_id()
        if work_id is None:
            return
        work = self.work_by_id.get(work_id)
        if not work:
            return
        for op_id in work.get("operations", []):
            op = self.operation_by_id.get(as_int(op_id))
            if not op:
                continue
            self.op_tree.insert(
                "",
                tk.END,
                iid=str(op_id),
                text=str(op_id),
                values=(
                    op.get("start", ""),
                    op.get("end", ""),
                    ",".join(str(x) for x in op.get("possible_tools", [])),
                    ",".join(str(x) for x in op.get("parents", [])),
                ),
            )

    def _refresh_detail_text(self):
        if not self.data:
            self._set_text(self.detail_text, "")
            return
        if self.selected_assignment:
            a = self.selected_assignment
            op = self.operation_by_id.get(as_int(a.get("operation_id")), {})
            work = self.work_by_id.get(as_int(a.get("work_id")), {})
            lines = [
                "Назначение",
                f"  исполнитель: {a.get('tool_id')}",
                f"  операция: {a.get('operation_id')}",
                f"  работа: {a.get('work_id')}",
                f"  интервал: {a.get('start')} - {a.get('end')}",
                f"  длительность: {as_int(a.get('end'), 0) - as_int(a.get('start'), 0)}",
                "",
                "Операция",
                f"  старт/конец: {op.get('start')} - {op.get('end')}",
                f"  прерываемая: {op.get('stoppable')}",
                f"  предшественники: {op.get('parents')}",
                f"  допустимые исполнители: {op.get('possible_tools')}",
                "",
                "Работа",
                f"  старт: {work.get('start')}",
                f"  директивный срок: {work.get('directive')}",
                f"  завершение: {work.get('completion')}",
                f"  опоздание: {work.get('tardiness')}",
                f"  штраф: {work.get('penalty')}",
            ]
        else:
            work_id = self._current_work_id()
            if work_id is None:
                lines = [
                    "Выберите работу в списке или блок на диаграмме Ганта.",
                    "",
                    "Цвет блока соответствует работе.",
                    "Серый фон показывает доступные окна исполнителя.",
                ]
            else:
                work = self.work_by_id.get(work_id, {})
                lines = [
                    f"Работа {work_id}",
                    f"  старт: {work.get('start')}",
                    f"  директивный срок: {work.get('directive')}",
                    f"  завершение: {work.get('completion')}",
                    f"  опоздание: {work.get('tardiness')}",
                    f"  коэффициент штрафа: {work.get('fine')}",
                    f"  штраф: {work.get('penalty')}",
                    f"  операции: {work.get('operations')}",
                ]
        self._set_text(self.detail_text, "\n".join(lines))

    def _set_text(self, widget: tk.Text, text: str):
        widget.configure(state=tk.NORMAL)
        widget.delete("1.0", tk.END)
        widget.insert("1.0", text)
        widget.configure(state=tk.DISABLED)

    def _on_work_combo(self, _event=None):
        self.selected_assignment = None
        work_id = self._current_work_id()
        if work_id is not None and self.work_tree.exists(str(work_id)):
            self.work_tree.selection_set(str(work_id))
            self.work_tree.see(str(work_id))
        else:
            self.work_tree.selection_remove(self.work_tree.selection())
        self.redraw_all()

    def _on_work_tree_select(self, _event=None):
        selection = self.work_tree.selection()
        if not selection:
            return
        work_id = as_int(selection[0])
        self.selected_assignment = None
        self.selected_work.set(f"Работа {work_id}")
        self.redraw_all()

    def _on_op_tree_select(self, _event=None):
        selection = self.op_tree.selection()
        if not selection:
            return
        op_id = as_int(selection[0])
        assignments = [
            a for a in self.assignments if as_int(a.get("operation_id")) == op_id
        ]
        if assignments:
            self.selected_assignment = assignments[0]
            self._refresh_detail_text()

    def _on_gantt_click(self, event):
        canvas = self.gantt_canvas
        x = canvas.canvasx(event.x)
        y = canvas.canvasy(event.y)
        items = canvas.find_overlapping(x, y, x, y)
        for item in reversed(items):
            tags = canvas.gettags(item)
            for tag in tags:
                assignment = getattr(canvas, "assignment_tags", {}).get(tag)
                if assignment:
                    self.selected_assignment = assignment
                    work_id = as_int(assignment.get("work_id"))
                    if work_id >= 0:
                        self.selected_work.set(f"Работа {work_id}")
                        if self.work_tree.exists(str(work_id)):
                            self.work_tree.selection_set(str(work_id))
                            self.work_tree.see(str(work_id))
                    self._refresh_operation_tree()
                    self._refresh_detail_text()
                    return

    def _on_canvas_wheel(self, event):
        widget = event.widget
        if event.state & 0x0001:
            widget.xview_scroll(int(-1 * (event.delta / 120)), "units")
        else:
            widget.yview_scroll(int(-1 * (event.delta / 120)), "units")

    def _current_work_id(self):
        value = self.selected_work.get()
        if value.startswith("Работа "):
            return as_int(value.split()[-1], None)
        return None

    def _current_tool_id(self):
        value = self.selected_tool.get()
        if value.startswith("Исполнитель "):
            return as_int(value.split()[-1], None)
        return None

    def _work_color(self, work_id: int) -> str:
        if work_id < 0:
            return "#8A94A6"
        return PALETTE[work_id % len(PALETTE)]

    def _max_time(self, tools, assignments):
        values = [1]
        for tool in tools:
            for _start, end in tool.get("schedule", []):
                values.append(as_int(end, 0))
        for assignment in assignments:
            values.append(as_int(assignment.get("end"), 0))
        return max(values)

    def _nice_tick_step(self, max_time):
        if max_time <= 0:
            return 1
        raw = max_time / 10
        magnitude = 10 ** int(math.floor(math.log10(raw)))
        normalized = raw / magnitude
        if normalized <= 1:
            nice = 1
        elif normalized <= 2:
            nice = 2
        elif normalized <= 5:
            nice = 5
        else:
            nice = 10
        return int(nice * magnitude)


def parse_args():
    parser = argparse.ArgumentParser(
        description="Visualize FactoryPlanningTools solution JSON."
    )
    parser.add_argument(
        "json_file",
        nargs="?",
        help="Path to SOLUTION_VISUALIZATION_V1 JSON file.",
    )
    return parser.parse_args()


def main():
    args = parse_args()
    initial_file = Path(args.json_file).resolve() if args.json_file else None
    app = ScheduleVisualizer(initial_file)
    app.mainloop()


if __name__ == "__main__":
    main()
