.venv: pyproject.toml
	uv sync
	touch .venv


test: .venv
	uv run python3 -m unittest tests/test_*.py


lint: .venv
	uv run flake8 ./reaktome


mypy: .venv
	uv run mypy ./reaktome


bump: .venv
	uv run bumpversion patch


version: .venv
	uv run bumpversion --dry-run --allow-dirty --list patch | grep current_version


package: .venv
	uv run python3 -m build --wheel


publish: package
	uv run twine upload dist/*


.PHONY: clean
clean:
	rm -rf dist
