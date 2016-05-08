from distutils.command.build_clib import build_clib
from distutils.core import setup
from distutils.extension import Extension
from Cython.Build import cythonize
from Cython.Distutils import build_ext
from os import listdir, path

pyx_src_dir = 'pyborist'
cc_src_dir = path.join('..', 'ArboristCore')

all_pyx_files = [x for x in listdir(pyx_src_dir) if x.endswith('.pyx')]
all_cpp_core_files = [path.join(cc_src_dir, x) 
    for x in listdir(cc_src_dir) if x.endswith('.cc')]
# special
all_cpp_core_files.extend([path.join(pyx_src_dir, 'callback.cc')])

lib_aborist_core = ('libaboristcore', 
    {
        'sources': all_cpp_core_files,
        'include_dirs':[cc_src_dir, pyx_src_dir]
    }
)

extensions = [
    Extension(x[:-4], [path.join(pyx_src_dir, x)],
        language = 'c++',
        include_dirs = [pyx_src_dir, cc_src_dir],
        library_dirs = [pyx_src_dir, cc_src_dir]
    ) for x in all_pyx_files
]


setup(
    libraries = [lib_aborist_core],
    cmdclass = {'build_clib': build_clib, 'build_ext': build_ext},
    ext_modules = cythonize(extensions)
)
