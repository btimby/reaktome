.venv/: pyproject.toml
	uv sync


test: .venv/
	uv run python3 -m unittest test_reaktome.py


lint: .venv/
	uv run flake8 ./reaktome


mypy: .venv/
	uv run mypy ./reaktome
