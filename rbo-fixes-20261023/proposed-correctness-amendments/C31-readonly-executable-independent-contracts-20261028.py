import ast,contextlib,fcntl,hashlib,json,os,signal,sys,tempfile,types
from pathlib import Path
from unittest.mock import patch
R=Path('/home/alexpaniman/projects/ydb/ai-verification/rbo-fixes-20261023')
D=R/'proposed-correctness-amendments'
sys.path.insert(0,str(R))
checks=[]
def check(value,name):
    if not value:raise AssertionError(name)
    checks.append(name)
def load(path,name):
    m=types.ModuleType(name);m.__file__=str(R/(name+'.py'))
    exec(compile(path.read_bytes(),str(path),'exec'),m.__dict__)
    return m
def tree(path):return ast.parse(path.read_text())
def function(t,name):return next(x for x in t.body if isinstance(x,ast.FunctionDef) and x.name==name)
def dump(t):return ast.dump(t,include_attributes=False)
gpath=D/'anonymous_cpp_guard_readonly_exec_20261028.candidate.py'
vpath=D/'anonymous_cpp_verify_readonly_exec_20261028.candidate.py'
guard=load(gpath,'anonymous_cpp_guard')
with patch.dict(sys.modules,{'anonymous_cpp_guard':guard}):
    direct=load(vpath,'anonymous_cpp_verify')
oldg,newg=tree(R/'anonymous_cpp_guard.py'),tree(gpath)
oldv,newv=tree(R/'anonymous_cpp_verify.py'),tree(vpath)
oldrun,newrun=function(oldg,'run_anonymous'),function(newg,'run_anonymous')
check(dump(oldrun.args)==dump(newrun.args),'guard_signature_and_default_budgets_unchanged')
for typ,name in [(ast.While,'scope_watch_loop')]:
    before=[dump(x) for x in ast.walk(oldrun) if isinstance(x,typ)]
    after=[dump(x) for x in ast.walk(newrun) if isinstance(x,typ)]
    check(before==after,name+'_unchanged')
before=next(x for x in oldrun.body if isinstance(x,ast.Try))
after=next(x for x in newrun.body if isinstance(x,ast.Try))
check([dump(x) for x in before.finalbody]==[dump(x) for x in after.finalbody],'scope_final_cleanup_unchanged')
for name in ('_consume','_environment_overrides'):
    check(dump(function(oldg,name))==dump(function(newg,name)),name+'_unchanged')
oldworker,newworker=function(oldg,'_worker'),function(newg,'_worker')
def calls(node,method):
    return [dump(x) for x in ast.walk(node) if isinstance(x,ast.Call) and
            isinstance(x.func,ast.Attribute) and x.func.attr==method]
check(calls(oldworker,'run')==calls(newworker,'run'),'worker_target_spawn_options_unchanged')
# run_anonymous calls are bare names.
def guard_calls(node):
    return [dump(x) for x in ast.walk(node) if isinstance(x,ast.Call) and isinstance(x.func,ast.Name) and x.func.id=='run_anonymous']
check(guard_calls(function(oldv,'execute'))==guard_calls(function(newv,'execute')),'all_direct_guard_calls_unchanged')
check(dump(function(oldv,'_fd_digest'))==dump(function(newv,'_fd_digest')),'exact_executable_digest_unchanged')
check(dump(function(oldv,'serial_slot'))==dump(function(newv,'serial_slot')),'serial_slot_unchanged')
check(dump(function(oldv,'validate_fixed_admission'))==dump(function(newv,'validate_fixed_admission')),'native_admission_not_weakened')

class Model:
    def __init__(self,mutation=None):
        self.live={41:dict(flags=os.O_RDWR,dev=3,ino=17,size=211,seals=0,memfd=True)}
        self.closed=[];self.events=[];self.mutation=mutation;self.masks=[]
    def control(self,fd,op,value=None):
        state=self.live[fd];self.events.append(('fcntl',fd,op))
        if op==fcntl.F_GETFL:return state['flags']
        if op==fcntl.F_GET_SEALS:return state['seals']
        if op==fcntl.F_ADD_SEALS:
            state['seals']=value
            return 0
        raise AssertionError('unexpected fcntl')
    def link(self,path):
        s=self.live[int(path.rsplit('/',1)[1])]
        return '/memfd:metadata-only (deleted)' if s['memfd'] else '/not-memfd'
    def open(self,path,flags):
        fd=max(self.live)+1
        self.live[fd]=dict(self.live[int(path.rsplit('/',1)[1])],flags=flags&os.O_ACCMODE)
        if self.mutation=='device':self.live[fd]['dev']+=1
        if self.mutation=='inode':self.live[fd]['ino']+=1
        if self.mutation=='size':self.live[fd]['size']+=1
        if self.mutation=='flags':self.live[fd]['flags']=os.O_RDWR
        if self.mutation=='seals':self.live[fd]['seals']=fcntl.F_SEAL_WRITE
        if self.mutation=='stat_interruption':self.live[fd]['interrupt']=True
        check(flags&os.O_CLOEXEC,'reopened_descriptor_cloexec')
        self.events.append(('open',fd))
        return fd
    def stat(self,fd):
        state=self.live[fd]
        if state.get('interrupt'):raise KeyboardInterrupt()
        return types.SimpleNamespace(st_dev=state['dev'],st_ino=state['ino'],st_size=state['size'])
    def close(self,fd):
        check(fd in self.live,'descriptor_closed_once')
        self.events.append(('close',fd));del self.live[fd];self.closed.append(fd)
    def mask(self,how,which):
        self.masks.append((how,set(which)))
        if how==signal.SIG_BLOCK:
            check(not self.events,'mask_before_descriptor_operations')
            return {signal.SIGUSR1}
        check(which=={signal.SIGUSR1},'prior_signal_mask_restored')
        if self.mutation=='pending_signal':
            check(41 in self.closed and len(self.live)==1,'pending_signal_after_old_writer_closed')
            raise KeyboardInterrupt()
        return set()
    @contextlib.contextmanager
    def mocked(self):
        with patch.object(os,'open',side_effect=self.open),patch.object(os,'fstat',side_effect=self.stat),\
             patch.object(os,'readlink',side_effect=self.link),patch.object(os,'close',side_effect=self.close),\
             patch.object(os,'fchmod'),patch.object(fcntl,'fcntl',side_effect=self.control),\
             patch.object(signal,'pthread_sigmask',side_effect=self.mask):
            yield

for mutation in (None,'device','inode','size','flags','seals','stat_interruption','pending_signal'):
    m=Model(mutation);owners={'executable':41}
    with m.mocked():
        try:record=direct._seal_native_executable(owners)
        except (direct.MetadataError,KeyboardInterrupt):
            check(mutation is not None,'unexpected_handoff_rejection')
            if mutation=='pending_signal':
                check(owners['executable']!=41 and m.live[owners['executable']]['flags']==os.O_RDONLY,'pending_signal_keeps_readonly_owner')
            else:
                check(owners=={'executable':41} and set(m.live)=={41},'failed_reopen_preserves_original_owner')
        else:
            check(mutation is None,'bad_identity_not_accepted')
            check(owners['executable']!=41 and 41 in m.closed,'new_mapping_and_no_writer')
            check(record['writable_owner_closed'] and record['same_inode'],'truthful_handoff_record')
        check(m.masks[0][0]==signal.SIG_BLOCK and m.masks[-1][0]==signal.SIG_SETMASK,'mask_restored_on_every_path')
        m.close(owners['executable'])
    check(not m.live,'outer_caller_cleanup_complete_'+str(mutation))

# The direct native call keeps compiler_environment=True. Both policies must
# pass transport checks; force the existing low-memory path before any spawn.
for policy in (True,False):
    for flags,seals,accepted in [(os.O_RDONLY,guard.NATIVE_EXECUTABLE_SEALS,True),
                               (os.O_RDWR,guard.NATIVE_EXECUTABLE_SEALS,False),
                               (os.O_RDONLY,fcntl.F_SEAL_WRITE,False)]:
        m=Model();m.live[41].update(flags=flags,seals=seals)
        with m.mocked(),patch.object(guard,'available_memory',return_value=0),\
             patch.object(guard.subprocess,'Popen',side_effect=AssertionError('No process allowed')):
            try:record,out=guard.run_anonymous(['@FD:executable@'],'/tmp',{'executable':41},compiler_environment=policy)
            except guard.AnonymousGuardError:check(not accepted,'parent_rejects_only_invalid_transport')
            else:
                check(accepted and record['exit_code']==75 and record['stop_reason']=='insufficient_starting_memory','valid_policy_reaches_existing_guard')
                check(record['native_executable_transport']=='sealed_read_only_memfd_v1' and out==b'','transport_record_with_no_native_claim')
        check(set(m.live)=={41},'parent_guard_does_not_own_caller_fd')

# Default environment policy at the scoped worker boundary and post-open failure.
for policy,bad_memfd in [(True,False),(False,False),(True,True)]:
    m=Model();m.live[41].update(flags=os.O_RDONLY,seals=guard.NATIVE_EXECUTABLE_SEALS)
    if bad_memfd:m.live[41]['memfd']=False
    spec=dict(argv=['@FD:executable@'],cwd='/tmp',fds={'executable':'/proc/1/fd/41'},
              read_only_fds=['executable'],compiler_environment=policy,environment={})
    invoked=[]
    def spawn(command,**kw):
        invoked.append(True)
        check(all(s['flags']==os.O_RDONLY for s in m.live.values()),'worker_has_no_writer')
        check(len(kw['pass_fds'])==1 and kw['pass_fds'][0]!=41,'worker_inherits_only_reopened_fd')
        if policy:check(kw['env']['TMPDIR']=='/proc/self/fd/anonymous-no-temp-directory','direct_native_sanitization_preserved')
        return types.SimpleNamespace(returncode=0)
    with m.mocked(),patch.object(Path,'read_bytes',return_value=json.dumps(spec).encode()),\
         patch.object(guard.resource,'setrlimit'),patch.object(guard.subprocess,'run',side_effect=spawn):
        try:result=guard._worker('/manifest/in-memory')
        except guard.AnonymousGuardError:check(bad_memfd,'worker_rejects_nonmemfd')
        else:check(not bad_memfd and result==0,'worker_default_policy_accepts')
    check(bool(invoked)==(not bad_memfd),'worker_spawn_only_after_validation')
    check(set(m.live)=={41},'worker_all_reopened_fds_closed')
report=dict(status='independent_source_and_mocked_descriptor_review_passed_native_pending',
            checks_passed=len(checks),checks=checks,actual_native_or_toolchain_execution=False,
            live_edits=False,generated_source_or_workload_persistence=False)
out=R/'verification/C31-readonly-executable-independent-metadata-20261028.json'
out.write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({'checks':len(checks),'report_sha256':hashlib.sha256(out.read_bytes()).hexdigest()}))
