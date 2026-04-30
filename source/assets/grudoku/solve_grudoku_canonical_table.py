#!/usr/bin/env python3
import argparse
import sys
from pathlib import Path

from z3 import Distinct, If, Int, Or, SolverFor, sat

ALPHABET = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
MAX_ORDER = len(ALPHABET)


class PuzzleError(ValueError):
    pass


def format_symbol(value: int) -> str:
    if value < 0 or value >= MAX_ORDER:
        raise PuzzleError(f"value out of range for output alphabet: {value}")
    return ALPHABET[value]


def parse_symbol(token: str) -> int:
    if len(token) == 1:
        symbol_index = ALPHABET.find(token)
        if symbol_index != -1:
            return symbol_index

    try:
        return int(token)
    except ValueError as exc:
        raise PuzzleError(f"invalid symbol: {token!r}") from exc


def parse_grid(text: str) -> list[list[int | None]]:
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    if not lines:
        raise PuzzleError("puzzle is empty")

    rows = [line.split() for line in lines]
    order = len(rows)
    width = len(rows[0])
    if any(len(row) != width for row in rows):
        raise PuzzleError("puzzle must be a rectangular grid")
    if width != order:
        raise PuzzleError("puzzle must be a square grid")
    if order > MAX_ORDER:
        raise PuzzleError(f"order must be <= {MAX_ORDER}")

    grid: list[list[int | None]] = []
    for row_index, row in enumerate(rows):
        parsed_row: list[int | None] = []
        for col_index, token in enumerate(row):
            if token == ".":
                parsed_row.append(None)
                continue
            try:
                value = parse_symbol(token)
            except ValueError as exc:
                raise PuzzleError(
                    f"invalid token at row {row_index}, col {col_index}: {token!r}"
                ) from exc
            if value < 0 or value >= order:
                raise PuzzleError(
                    f"value out of range at row {row_index}, col {col_index}: {value}"
                )
            parsed_row.append(value)
        grid.append(parsed_row)

    expected_header = list(range(order))
    if grid[0] != expected_header:
        raise PuzzleError(
            f"row 0 must be: {' '.join(format_symbol(value) for value in expected_header)}"
        )

    for row_index in range(order):
        header_value = grid[row_index][0]
        if header_value != row_index:
            raise PuzzleError(f"column 0 must be {row_index} at row {row_index}")

    return grid


def collect_solver_stats(solver) -> dict[str, int | float]:
    return {key: value for key, value in solver.statistics()}


def format_grid(grid: list[list[int]]) -> str:
    return "\n".join(" ".join(format_symbol(value) for value in row) for row in grid)


def read_input(path: str | None) -> str:
    if path is None:
        return sys.stdin.read()
    return Path(path).read_text(encoding="utf-8")


def print_stats(stats: dict[str, dict[str, int | float]]) -> None:
    for phase in ("solve", "uniqueness"):
        print(f"[{phase}]")
        for key in sorted(stats[phase]):
            print(f"{key}: {stats[phase][key]}")


CANONICAL_TABLE_STRINGS: dict[int, list[tuple[str, str]]] = {
    8: [
        ("cyclic", "0123456712453670243657013560712443576012567102436702143570142356"),
        ("c4_x_c2", "0123456713456072240617353560712446173250507123466732540172540613"),
        ("dihedral", "0123456710452376270654313560712446173250537160426432170572540613"),
        ("quaternion", "0123456713456072273614053560712442573610507123466402573176140253"),
        ("c2_x_c2_x_c2", "0123456710452376240617353560712442170653537160426732540176543210"),
    ],
    10: [
        ("cyclic", "0123456789103254769823456789013254769810456789012354769810326789012345769810325489012345679810325476"),
        ("dihedral", "0123456789103254769829416385073850729416476981032556789012346587092143749618305283052749619214365870"),
    ],
    12: [
        ("dicyclic", "0123456789AB124506893AB72406183A5B793967B1A045284018235B679A5A8972B106436B3A9472180575A08923B164875BA094231696B13A457280A8725B069431B3946718A052"),
        ("cyclic", "0123456789AB13457089A2B62467890A1B35357091A2B4684789A21B3650509123B4678A680A1B23457979A2B43658018A1B3645709292B467580A13AB3658709124B6580A912347"),
        ("a4", "0123456789AB1456089A23B72607BA1398543A7086524B194089123B567A591A7B4632806BA129850743753291A0B468834BA709652197B453281A06A86534B17092B2986074A135"),
        ("dihedral", "0123456789AB1045238967BA2406183A5B793967B1A045284218065B3A975789A0B123466B3A9472180575A08923B1648A5B7294061393B16745A082A8725B069431B6943A187250"),
        ("c6_x_c2", "0123456789AB1045238967BA2406183A5B79356789A0B1244218065B3A97538967B1A042683A5B72940179A0B1234568865B3A94721097B1A0452386AB7294061835BA9472180653"),
    ],
}

SUPPORTED_ORDERS = frozenset(CANONICAL_TABLE_STRINGS)


def parse_serialized_table(order: int, serialized: str) -> list[list[int]]:
    if len(serialized) != order * order:
        raise PuzzleError(
            f"canonical table length mismatch for order {order}: {len(serialized)}"
        )

    def parse_symbol(symbol: str) -> int:
        if "0" <= symbol <= "9":
            return ord(symbol) - ord("0")
        if "A" <= symbol <= "Z":
            return ord(symbol) - ord("A") + 10
        if "a" <= symbol <= "z":
            return ord(symbol) - ord("a") + 36
        raise PuzzleError(f"invalid canonical table symbol: {symbol!r}")

    values = [parse_symbol(symbol) for symbol in serialized]
    return [values[offset : offset + order] for offset in range(0, len(values), order)]


FAMILY_SPECS: dict[int, list[tuple[str, list[list[int]]]]] = {
    order: [(name, parse_serialized_table(order, serialized)) for name, serialized in specs]
    for order, specs in CANONICAL_TABLE_STRINGS.items()
}


def merge_stats(stat_sets: list[dict[str, int | float | str]]) -> dict[str, int | float | str]:
    merged: dict[str, int | float | str] = {}
    for stats in stat_sets:
        for key, value in stats.items():
            if key in {"max memory", "memory"} and isinstance(value, (int, float)):
                current = merged.get(key, 0)
                if not isinstance(current, (int, float)) or value > current:
                    merged[key] = value
            elif isinstance(value, (int, float)) and isinstance(merged.get(key, 0), (int, float)):
                merged[key] = merged.get(key, 0) + value
            elif key not in merged:
                merged[key] = value
    return merged


def pick(values, index):
    expr = values[-1]
    for position in range(len(values) - 2, -1, -1):
        expr = If(index == position, values[position], expr)
    return expr


def table_mul(table: list[list[int]], left, right):
    return pick([pick(row, right) for row in table], left)


def build_family_solver(
    grid: list[list[int | None]],
    family_name: str,
    table: list[list[int]],
):
    order = len(table)
    solver = SolverFor("QF_FD")
    label_to_element = [Int(f"{family_name}_elem_{label}") for label in range(order)]
    cells = [[Int(f"{family_name}_cell_{row}_{col}") for col in range(order)] for row in range(order)]

    for label in range(order):
        solver.add(label_to_element[label] >= 0, label_to_element[label] < order)
    solver.add(Distinct(label_to_element))
    solver.add(label_to_element[0] == 0)

    for row in range(order):
        for col in range(order):
            solver.add(cells[row][col] >= 0, cells[row][col] < order)
        solver.add(Distinct(cells[row]))
        solver.add(Distinct([cells[col][row] for col in range(order)]))
        solver.add(cells[0][row] == row)
        solver.add(cells[row][0] == row)

    for row in range(order):
        for col in range(order):
            target = table_mul(table, label_to_element[row], label_to_element[col])
            solver.add(pick(label_to_element, cells[row][col]) == target)
            clue = grid[row][col]
            if clue is not None:
                solver.add(cells[row][col] == clue)

    return solver, cells


def solve_family(
    grid: list[list[int | None]],
    family_name: str,
    table: list[list[int]],
):
    order = len(table)
    solver, cells = build_family_solver(grid, family_name, table)
    status = solver.check()
    stats = collect_solver_stats(solver)
    if status != sat:
        return {
            "family": family_name,
            "status": status,
            "stats": stats,
        }

    model = solver.model()
    solution = [
        [model.evaluate(cells[row][col], model_completion=True).as_long() for col in range(order)]
        for row in range(order)
    ]
    return {
        "family": family_name,
        "status": status,
        "stats": stats,
        "solver": solver,
        "cells": cells,
        "solution": solution,
    }


def uniqueness_stats_for_family(result) -> tuple[bool, dict[str, int | float | str]]:
    solver = result["solver"]
    cells = result["cells"]
    solution = result["solution"]
    order = len(solution)

    solver.push()
    solver.add(Or([cells[row][col] != solution[row][col] for row in range(order) for col in range(order)]))
    unique = solver.check() != sat
    stats = collect_solver_stats(solver)
    solver.pop()
    return unique, stats


def solve_grid_detailed(
    grid: list[list[int | None]],
) -> tuple[list[list[int]], bool, str, dict[str, dict[str, int | float | str]]]:
    order = len(grid)
    if order not in SUPPORTED_ORDERS:
        supported = ", ".join(str(value) for value in sorted(SUPPORTED_ORDERS))
        raise PuzzleError(f"known-family solver only supports orders: {supported}")

    family_runs = [solve_family(grid, family_name, table) for family_name, table in FAMILY_SPECS[order]]
    sat_runs = [run for run in family_runs if str(run["status"]) == "sat"]
    uniqueness_checks = [uniqueness_stats_for_family(run) for run in sat_runs]

    solve_stats = merge_stats([run["stats"] for run in family_runs])

    if not sat_runs:
        raise PuzzleError("unsat")

    chosen = sat_runs[0]
    solution = chosen["solution"]
    family = chosen["family"]

    if len(sat_runs) >= 2 and any(run["solution"] != solution for run in sat_runs[1:]):
        return solution, False, family, {
            "solve": solve_stats,
            "uniqueness": {},
        }

    unique = all(flag for flag, _ in uniqueness_checks)
    uniqueness_stats = merge_stats([stats for _, stats in uniqueness_checks])

    return solution, unique, family, {
        "solve": solve_stats,
        "uniqueness": uniqueness_stats,
    }


def solve_grid(grid: list[list[int | None]]) -> tuple[list[list[int]], bool, str]:
    solution, unique, family, _ = solve_grid_detailed(grid)
    return solution, unique, family


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Solve order-8, order-10, and order-12 grudoku puzzles using known group families."
    )
    parser.add_argument("path", nargs="?", help="Puzzle file. Reads stdin if omitted.")
    parser.add_argument(
        "--stats",
        action="store_true",
        help="Print Z3 statistics for the initial solve and uniqueness check.",
    )
    args = parser.parse_args(argv)

    try:
        grid = parse_grid(read_input(args.path))
        solution, unique, family, stats = solve_grid_detailed(grid)
    except PuzzleError as exc:
        if str(exc) == "unsat":
            print("unsat")
        else:
            print(f"error: {exc}", file=sys.stderr)
        return 1
    except OSError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    print(format_grid(solution))
    print(f"unique: {'yes' if unique else 'no'}")
    print(f"family: {family}")
    if args.stats:
        print_stats(stats)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
