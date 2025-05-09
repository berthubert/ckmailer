project('ckmailer', 'cpp', default_options : ['cpp_std=c++20'])

sqlitedep = dependency('sqlite3', version : '>=3.48')
thread_dep = dependency('threads')
json_dep = dependency('nlohmann_json')
bcryptcpp_dep = dependency('bcryptcpp', static: true)
pugi_dep = dependency('pugixml')
fmt_dep = dependency('fmt', version: '>=9.1.0', static: true)
simplesockets_dep = dependency('simplesockets', static: true)
cpphttplib = dependency('cpp-httplib')
sqlitewriter_dep = dependency('sqlitewriter', static: true)
doctest_dep=dependency('doctest')
argparse_dep = dependency('argparse', version: '>=3')

vcs_ct=vcs_tag(command: ['git', 'describe', '--tags', '--always', '--dirty', '--abbrev=9'], 
      input:'git_version.h.in',
      output:'git_version.h',
      replace_string:'@GIT_VERSION@')

vcs_dep= declare_dependency (sources: vcs_ct)

executable('ckm', 'ckmailer.cc',  'support.cc', 'nonblocker.cc', 'imap.cc', 
	dependencies: [sqlitedep, json_dep, fmt_dep, simplesockets_dep, cpphttplib, sqlitewriter_dep,
	argparse_dep, vcs_dep])

executable('ckmserv', 'ckmserv.cc',  'support.cc', 'nonblocker.cc', 'imap.cc', 
	dependencies: [sqlitedep, json_dep, fmt_dep, simplesockets_dep, cpphttplib, sqlitewriter_dep,
	argparse_dep, vcs_dep, pugi_dep])

executable('testrunner', 'testrunner.cc', 'support.cc',  
dependencies: [sqlitedep, json_dep, fmt_dep, sqlitedep, sqlitewriter_dep, doctest_dep, cpphttplib, simplesockets_dep])
