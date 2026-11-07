#!/usr/bin/env python3
"""Generate the explicit OHOS identity delta against a fixed Aurora source tree."""
import argparse
import difflib
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('source', type=Path)
p.add_argument('output', type=Path)
a = p.parse_args()
patch = []


def modify(name, edits):
    before = (a.source / name).read_text()
    after = before
    for old, new in edits:
        if after.count(old) != 1:
            raise ValueError(f'{name}: expected one unchanged anchor: {old!r}')
        after = after.replace(old, new, 1)
    patch.extend(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                                    fromfile='a/' + name, tofile='b/' + name))


modify('include/linux/sched.h', [
    ('\t/* CPU-specific state of this task: */',
     '#ifdef CONFIG_ACCESS_TOKENID\n\tu64 token;\n\tu64 ftoken;\n#endif\n\n'
     '\t/* CPU-specific state of this task: */')])
modify('drivers/android/binder_internal.h', [
    ('\tstruct binder_extended_error ee;',
     '\tstruct binder_extended_error ee;\n#ifdef CONFIG_ACCESS_TOKENID\n'
     '\tstruct access_token tokens;\n\tu64 sender_pid_nr;\n#endif'),
    ('\tkuid_t  sender_euid;', '\tkuid_t  sender_euid;\n#ifdef CONFIG_ACCESS_TOKENID\n'
     '\tu64 sender_tokenid;\n\tu64 first_tokenid;\n#endif')])

uapi = '''/* OpenHarmony 6.0 Binder identity extensions. */
struct binder_feature_set {
    __u64 feature_set;
};
struct access_token {
    __u64 sender_tokenid;
    __u64 first_tokenid;
    __u64 reserved[2];
};
struct binder_sender_info {
    struct access_token tokens;
    __u64 sender_pid_nr;
};
#define BINDER_FEATURE_SET _IOWR('b', 30, struct binder_feature_set)
#define BINDER_GET_ACCESS_TOKEN _IOWR('b', 31, struct access_token)
#define BINDER_GET_SENDER_INFO _IOWR('b', 32, struct binder_sender_info)

'''
modify('include/uapi/linux/android/binder.h', [
    ('enum {\n\tBINDER_WRITE_READ', uapi + 'enum {\n\tBINDER_WRITE_READ')])

ioctl = '''\tcase BINDER_FEATURE_SET: {
\t\tstruct binder_feature_set features = { 0 };
#ifdef CONFIG_ACCESS_TOKENID
\t\t/* Official feature bits: AccessToken=0, sender-info=2. */
\t\tfeatures.feature_set = (1ULL << 0) | (1ULL << 2);
#endif
\t\tif (copy_to_user(ubuf, &features, sizeof(features))) {
\t\t\tret = -EFAULT;
\t\t\tgoto err;
\t\t}
\t\tbreak;
\t}
#ifdef CONFIG_ACCESS_TOKENID
\tcase BINDER_GET_ACCESS_TOKEN: {
\t\tstruct access_token tokens = { 0 };
\t\tbinder_inner_proc_lock(proc);
\t\ttokens.sender_tokenid = thread->tokens.sender_tokenid;
\t\ttokens.first_tokenid = thread->tokens.first_tokenid;
\t\tbinder_inner_proc_unlock(proc);
\t\tif (copy_to_user(ubuf, &tokens, sizeof(tokens))) {
\t\t\tret = -EFAULT;
\t\t\tgoto err;
\t\t}
\t\tbreak;
\t}
\tcase BINDER_GET_SENDER_INFO: {
\t\tstruct binder_sender_info sender = { 0 };
\t\tbinder_inner_proc_lock(proc);
\t\tsender.tokens.sender_tokenid = thread->tokens.sender_tokenid;
\t\tsender.tokens.first_tokenid = thread->tokens.first_tokenid;
\t\tsender.sender_pid_nr = thread->sender_pid_nr;
\t\tbinder_inner_proc_unlock(proc);
\t\tif (copy_to_user(ubuf, &sender, sizeof(sender))) {
\t\t\tret = -EFAULT;
\t\t\tgoto err;
\t\t}
\t\tbreak;
\t}
#endif
'''
modify('drivers/android/binder.c', [
    ('\tt->sender_euid = current_euid();',
     '\tt->sender_euid = current_euid();\n#ifdef CONFIG_ACCESS_TOKENID\n'
     '\tt->sender_tokenid = READ_ONCE(current->token);\n'
     '\tt->first_tokenid = READ_ONCE(current->ftoken);\n#endif'),
    ('\t\tt->buffer->allow_user_free = 1;',
     '\t\tt->buffer->allow_user_free = 1;\n#ifdef CONFIG_ACCESS_TOKENID\n'
     '\t\tbinder_inner_proc_lock(proc);\n'
     '\t\tthread->tokens.sender_tokenid = t->sender_tokenid;\n'
     '\t\tthread->tokens.first_tokenid = t->first_tokenid;\n'
     '\t\tthread->sender_pid_nr = t->from_pid;\n'
     '\t\tbinder_inner_proc_unlock(proc);\n#endif'),
    ('\tcase BINDER_VERSION: {', ioctl + '\tcase BINDER_VERSION: {')])
modify('drivers/Kconfig', [
    ('source "drivers/android/Kconfig"',
     'source "drivers/android/Kconfig"\nsource "drivers/accesstokenid/Kconfig"')])
before = (a.source / 'drivers/Makefile').read_text()
after = before + '\nobj-$(CONFIG_ACCESS_TOKENID) += accesstokenid/\n'
patch.extend(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                                fromfile='a/drivers/Makefile', tofile='b/drivers/Makefile'))
a.output.write_text(''.join(patch))
print(f'Generated {a.output}: {len(patch)} diff lines')
