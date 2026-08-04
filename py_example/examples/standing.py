#!/usr/bin/env python3
"""Capture standing robot(s) without sending motor commands."""

from move_head import main


if __name__ == "__main__":
    raise SystemExit(main(default_mode="static"))
