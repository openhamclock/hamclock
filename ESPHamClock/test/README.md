# HamClock Test Suite

Quick reference for running the automated menu and UI test suite.

## Running Tests

Attach to an already running HamClock web instance:

```bash
python3 hctest.py run --url http://127.0.0.1:8080
```

> **Note:** If running the Playwright-based variant (`hctest_v1.py`):
> ```bash
> python3 hctest_v1.py run --url http://127.0.0.1:8080 --driver playwright
> ```

Or let the test harness launch and manage its own headless binary:

```bash
python3 hctest.py run --launch ./hamclock-web-800x480 --mock
```

## Help & Options

Detailed options and command help:

```bash
python3 hctest.py -h
python3 hctest.py run -h
```

### Common Commands

* **`run`** – Run menu and UI tests across chrome elements and pane choices:
  * `--suite {all,chrome,panes}` – Select test suite (default: `all`)
  * `--only <name>` – Filter to tests matching a string
  * `--panes <list>` – Test specific comma-separated pane choices
  * `--deep` – Tap each row inside menus
* **`discover`** – Scan the screen to discover tap spots and menu entry points.
* **`onta`** – Test On the Air scroll and refresh behaviors.
* **`mock`** – Run a standalone mock backend server.
