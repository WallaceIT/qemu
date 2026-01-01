executable('vhost-user-can', files('vhost-user-can.c'),
           dependencies: [qemuutil, vhost_user],
           build_by_default: host_os == 'linux',
           install: false)
