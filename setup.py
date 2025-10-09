from setuptools import setup, Extension, find_packages

setup(
    name="reaktome",
    version="0.1.14",
    description="Advisory-only setattr hooks with veto support",
    packages=find_packages(include=["reaktome", "reaktome.*"]),
    package_data={
        'reaktome': ['py.typed'],
    },
    ext_modules=[
        Extension(
            name="_reaktome",
            sources=[
                "src/reaktome.c",
                "src/list.c",
                "src/dict.c",
                "src/set.c",
                "src/obj.c",
                "src/activation.c",
            ],
            include_dirs=["src"],  # <— tells gcc where to find reaktome.h
        )
    ],
    python_requires=">=3.12",
)
