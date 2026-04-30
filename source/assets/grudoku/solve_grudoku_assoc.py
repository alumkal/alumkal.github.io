#!/usr/bin/env python3
import argparse
import sys
from pathlib import Path

from z3 import Distinct, Function, IntSort, Or, Solver, sat

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


def collect_solver_stats(solver: Solver) -> dict[str, int | float]:
    return {key: value for key, value in solver.statistics()}


def solve_grid_detailed(
    grid: list[list[int | None]],
) -> tuple[list[list[int]], bool, dict[str, dict[str, int | float]]]:
    order = len(grid)
    op = Function("op", IntSort(), IntSort(), IntSort())
    solver = Solver()

    for left in range(order):
        for right in range(order):
            solver.add(op(left, right) >= 0, op(left, right) < order)

    for element in range(order):
        solver.add(op(0, element) == element)
        solver.add(op(element, 0) == element)
        solver.add(Distinct([op(element, other) for other in range(order)]))
        solver.add(Distinct([op(other, element) for other in range(order)]))

    for row in range(1, order):
        for col in range(1, order):
            clue = grid[row][col]
            if clue is not None:
                solver.add(op(row, col) == clue)

    for a in range(order):
        for b in range(order):
            for c in range(order):
                solver.add(op(a, op(b, c)) == op(op(a, b), c))

    if solver.check() != sat:
        raise PuzzleError("unsat")
    solve_stats = collect_solver_stats(solver)

    model = solver.model()
    solution = [
        [model.evaluate(op(row, col), model_completion=True).as_long() for col in range(order)]
        for row in range(order)
    ]

    solver.push()
    solver.add(Or([op(row, col) != solution[row][col] for row in range(order) for col in range(order)]))
    unique = solver.check() != sat
    uniqueness_stats = collect_solver_stats(solver)
    solver.pop()

    return solution, unique, {
        "solve": solve_stats,
        "uniqueness": uniqueness_stats,
    }


def solve_grid(grid: list[list[int | None]]) -> tuple[list[list[int]], bool]:
    solution, unique, _ = solve_grid_detailed(grid)
    return solution, unique


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


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Solve grudoku puzzles with Z3.")
    parser.add_argument("path", nargs="?", help="Puzzle file. Reads stdin if omitted.")
    parser.add_argument(
        "--stats",
        action="store_true",
        help="Print Z3 statistics for the initial solve and uniqueness check.",
    )
    args = parser.parse_args(argv)

    try:
        grid = parse_grid(read_input(args.path))
        solution, unique, stats = solve_grid_detailed(grid)
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
    if args.stats:
        print_stats(stats)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
