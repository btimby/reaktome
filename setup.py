from setuptools import setup, Extension, find_packages

setup(
    name="reaktome",
    version="0.1.3",
    description="Advisory-only setattr hooks with veto support",
    packages=find_packages(include=["reaktome", "reaktome.*"]),
    ext_modules=[
        Extension(
            name="_reaktome",
            sources=["src/_reaktome.c"],
        )
    ],
    python_requires=">=3.12",
)
