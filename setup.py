from setuptools import setup, Extension, find_packages
import sysconfig

python_ldflags = sysconfig.get_config_var("LDFLAGS") or ""
python_libs = sysconfig.get_config_var("LIBS") or ""
python_ldlibrary = sysconfig.get_config_var("LDLIBRARY") or ""
python_libdir = sysconfig.get_config_var("LIBDIR") or ""
python_ldargs = [flag for flag in python_ldflags.split() if flag]

setup(
    name="reaktome",
    version="0.1.18",
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
            library_dirs=[python_libdir] if python_libdir else [],
            extra_link_args=python_ldargs + [f"-lpython{sysconfig.get_python_version()}"],
        ),
    ],
    python_requires=">=3.12",
)
