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


publish: .venv
	uv run twine upload dist/reaktome-*-manylinux?_*_*.whl


build: .venv
	uv pip install -e .


.PHONY: clean
clean:
	rm -rf build/ dist/ *.egg-info
	find . -name "_reaktome*.so" -delete	
