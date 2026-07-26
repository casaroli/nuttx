=========================================
Migrating to separate ``fork``/``vfork``
=========================================

What changed
============

NuttX used to implement ``fork()`` and ``vfork()`` as the same function.  Both
were thin libc wrappers around a single ``up_fork()`` syscall; ``vfork()``
differed only by a trailing ``waitpid()``.  Underneath, the child joined the
parent's address environment -- the same ``addrenv_join()`` that
``pthread_create()`` uses -- and got a private *copy of the stack*.  So the
child shared ``.data``, ``.bss`` and the heap with its parent, and ran
concurrently with it.

That is not ``fork()``.  It is ``vfork()``-with-a-private-stack published under
``fork()``'s name.  The history says so plainly: today's ``fork()`` is NuttX's
old ``vfork()``, renamed in 2023 without any change of behaviour.  And the
consequence was silent: a program written against POSIX ``fork()`` compiled and
ran, and its child's writes quietly landed in the parent's variables.

There are now three distinct primitives:

.. list-table::
   :header-rows: 1
   :widths: 14 30 26 30

   * - API
     - Memory
     - Parent
     - Availability
   * - ``fork()``
     - child gets **its own copy** at the same virtual addresses
     - runs concurrently
     - ``CONFIG_ARCH_HAVE_FORK`` -- only where an address environment can be
       duplicated
   * - ``vfork()``
     - child **shares** the parent's memory
     - **suspended** until the child ``_exit()``\ s or ``exec()``\ s
     - ``CONFIG_ARCH_HAVE_VFORK`` -- everywhere, MMU or not
   * - ``task_fork()``
     - child shares memory, private stack copy
     - runs concurrently
     - ``CONFIG_ARCH_HAVE_TASK_FORK`` -- exactly where ``fork()`` existed
       before

``task_fork()`` is the old behaviour under an honest name.  Nothing was lost.

This is a breaking change
=========================

Two things break, and they break loudly rather than quietly:

**Code calling** ``fork()`` **on a target without a duplicable address
environment no longer builds.**  ``fork()`` is not declared in ``unistd.h``
there, so you get a compile error naming the function.  That is the intended
outcome:  a build error is strictly better than the silent wrongness it
replaces.

**Code calling** ``fork()`` **on a target that does have real** ``fork()``
**changes behaviour** -- from sharing to copying.  Code that (perhaps
unknowingly) relied on the sharing will now see the parent and child diverge.

Which replacement do I want?
============================

Answer the question "why did I call ``fork()``?".

*I want the child to run a different program.*
   Use :c:func:`posix_spawn` or ``task_spawn()``.  This is the single most
   common reason to call ``fork()``, NuttX has always provided a better answer
   for it, and that answer does not have the pid discontinuity that
   ``fork()``\ +\ ``exec()`` has.  If you must keep the two-step idiom, use
   ``vfork()`` + ``exec*()``:  that is exactly what ``vfork()`` is for, and it
   is available on every target.

*I want a second flow of control that shares my memory.*
   Use ``pthread_create()``.  That is the same memory relationship, spelled
   clearly, with a normal entry point instead of a function that returns twice.
   If you specifically need the returns-twice shape -- for example you are
   porting code and do not want to restructure it -- use ``task_fork()``.  It
   is a rename, not a rewrite:

   .. code-block:: c

      #include <sched.h>

      pid = task_fork();     /* was: pid = fork(); */

*I want a genuinely independent copy of this process.*
   Keep calling ``fork()``, and make sure your configuration selects
   ``CONFIG_ARCH_HAVE_FORK``.  Be aware there is no copy-on-write: the copy is
   eager, so forking a large process needs as much free memory as the process
   occupies and fails with ``ENOMEM`` otherwise.

*I cannot change the code right now.*
   Set ``CONFIG_FORK_IS_TASK_FORK=y``.  This aliases ``fork()`` back to
   ``task_fork()``, restoring the previous behaviour **exactly** -- same
   sharing, same concurrency, no new suspension.  It is available on precisely
   the configurations that had ``fork()`` before, and it depends on
   ``!ARCH_HAVE_FORK``: on a target that can provide real ``fork()``, aliasing
   it back to sharing would reintroduce the very ambiguity this change removes,
   so sharing-dependent callers there must be edited.

Configuration symbols
=====================

``CONFIG_ARCH_HAVE_TASK_FORK``
   Hidden.  The architecture can clone the calling task with a copied stack.
   Inherits exactly the ``select`` lines that ``ARCH_HAVE_FORK`` used to have,
   so no configuration that had ``fork()`` loses the machinery.

``CONFIG_ARCH_HAVE_VFORK``
   Hidden.  The architecture can implement POSIX ``vfork()``.

``CONFIG_ARCH_VFORK_STACK_BORROW`` / ``CONFIG_ARCH_VFORK_STACK_RESERVE``
   Hidden / tunable.  The ``vfork()`` child borrows the parent's stack rather
   than running on a relocated copy of it.  Required on architectures whose
   stack frames hold absolute stack addresses -- the windowed ABI of Xtensa,
   whose register-window save areas hold each frame's spilled stack pointer, so
   that a relocated copy unwinds onto the parent's stack.  The reserve is the
   headroom withheld from the child for the parent's own remaining frames; a
   canary at the boundary turns an undersized reserve into a loud failure.

``CONFIG_ARCH_HAVE_ADDRENV_FORK``
   Hidden.  The architecture implements ``up_addrenv_fork()``, which duplicates
   an address environment into freshly allocated pages mapped at the same
   virtual addresses.

``CONFIG_ARCH_HAVE_FORK``
   Hidden, derived: ``ARCH_ADDRENV && ARCH_HAVE_ADDRENV_FORK``.  It no longer
   means "``fork()`` exists"; it means "this configuration can provide POSIX
   ``fork()`` semantics".

``CONFIG_FORK_IS_TASK_FORK``
   Visible, default ``n``.  The legacy alias described above.

Notes for architecture maintainers
==================================

The register/stack snapshot machinery is common to all three primitives.  Each
architecture exposes three entry points -- ``up_task_fork()``, ``up_vfork()``
and ``up_fork()`` -- which share one snapshot sequence and differ only in a
``FORK_TYPE_*`` selector (see ``include/nuttx/fork.h``) handed to
``nxtask_setup_fork()``.  That is where the memory semantics are decided:
``addrenv_join()`` for ``task_fork()`` and ``vfork()``, ``addrenv_fork()`` for
``fork()``.

To add real ``fork()`` to an architecture, two things are needed.  First,
implement ``up_addrenv_fork()`` and add ``ARCH_HAVE_ADDRENV_FORK`` to the
``default y if`` list in ``arch/Kconfig``.  Second -- and this is the part that
is easy to miss -- in a kernel or protected build all three primitives arrive
through a system call, so the return address and stack pointer the
architecture's fork entry point can observe for itself belong to the *kernel*,
not to the caller.  A child built from those resumes at a kernel address on a
kernel stack.

The architecture must therefore record the caller's exception frame when it
traps and build the child from that instead.  Three architectures do it, and
they are worth copying:

* RISC-V: ``riscv_swint.c`` stores the frame in ``xcp.sregs``, and
  ``riscv_fork.c`` has a ``CONFIG_LIB_SYSCALL`` variant of ``riscv_fork()``
  that rebuilds the child from it.
* arm64: ``arm64_vectors.S`` hands the frame to ``dispatch_syscall()``, which
  stores it in ``xcp.sregs``; ``arm64_fork()`` then dispatches to
  ``arm64_fork_syscall()`` or ``arm64_fork_direct()`` according to whether
  ``TCB_FLAG_SYSCALL`` is set, so a kernel thread that calls the entry point
  directly still works.
* armv7-a: ``arm_syscall()`` stores the frame in ``xcp.sregs``, and
  ``arm_fork()`` dispatches to ``arm_fork_syscall()`` or
  ``arm_fork_direct()``.  Note that the discriminator here is a saved user
  stack pointer, ``xcp.ustkptr``, not ``TCB_FLAG_SYSCALL``:  armv7-a
  dispatches a system call by re-pointing the caller's own exception frame at
  ``dispatch_syscall()``, so the caller *is* the task that runs the kernel
  side of the call.  What makes its snapshot useless is not the system call
  as such but the switch to the kernel stack, which leaves the kernel-side
  frames on a stack the child gets no copy of.  A protected build without a
  kernel stack dispatches on the caller's own stack, so there the frames are
  copied along with the caller's and ``arm_fork_direct()`` remains correct.
  Because ``arm_syscall()`` has already re-pointed the frame by the time
  ``arm_fork()`` runs, its PC, CPSR and SP are the kernel's; the caller's are
  read from where ``arm_syscall()`` put them -- ``syscall[0].sysreturn``,
  ``syscall[0].cpsr`` and ``ustkptr``.

* x86_64: ``x86_64_syscall()`` stores the frame in ``xcp.sregs``, and
  ``x86_64_fork()`` dispatches to ``x86_64_fork_syscall()`` or
  ``x86_64_fork_direct()``.  The discriminator here is ``xcp.sregs`` itself
  being non-NULL, because raising ``TCB_FLAG_SYSCALL`` would also defer signal
  actions -- something x86_64 has never done and its kernel-build signal path
  does not currently survive.  Two properties of ``SYSCALL``/``SYSRET`` shape
  the child's frame:  the instruction leaves the caller's RIP and RFLAGS in
  RCX and R11 rather than on a stack, so they have to be moved into the RIP
  and RFLAGS slots of the interrupt frame the child is resumed from; and the
  hardware never records the caller's CS and SS at all -- ``SYSRETQ``
  reconstructs them from ``IA32_STAR`` -- so the child's have to be filled in
  with the user code and data selectors at RPL 3.  For the same reason the
  saved frame is not copied wholesale:  only the extended state and the
  general registers are inherited, and the segment registers and thread
  pointer come from the frame ``up_initial_state()`` built for the child.

Nothing else is required:  the ``up_fork()`` entry point and the libc wrapper
are already there and become live automatically.

Known gaps
==========

**RISC-V, arm64, armv7-a and x86_64 select** ``ARCH_HAVE_ADDRENV_FORK``, so
their kernel builds have ``fork()``.  Every architecture with an MMU address
environment and the syscall-frame path described above is covered; what is
left is architectures that have neither.

The *protected* configurations are excluded deliberately rather than
left unimplemented, and that holds whether the protection comes from an MPU
(ARMv8-R) or from a static set of MMU mappings (``qemu-armv8a:pnsh``,
``qemu-armv7a:pnsh``).  A
protected build has one address space carved up once at boot; its
``up_addrenv_*()`` are stubs, and there is no mapping to duplicate at the same
virtual addresses, so POSIX ``fork()`` semantics cannot be provided at all.
``vfork()`` and ``task_fork()``, which share the parent's memory, work there as
everywhere else.  This is why ``ARCH_HAVE_ADDRENV_FORK`` carries
``&& !BUILD_PROTECTED``:  ``ARCH_ADDRENV`` being set is not by itself evidence
that address environments are real.

**Xtensa has neither** ``fork()`` **nor** ``vfork()``.  It never had ``fork()``
either -- ``ARCH_HAVE_FORK`` was never selected for it -- because the hybrid
NuttX used to implement (shared memory, relocated stack copy) cannot work on a
windowed ABI:  the register-window save areas embedded in the stack hold
absolute stack pointers, so a child started on a relocated copy reloads a
stack pointer into the *parent's* stack on its very first window underflow.

The generic half of the fix is in place:  ``CONFIG_ARCH_VFORK_STACK_BORROW``
and ``vfork_borrow_stack()`` in ``sched/task/task_fork.c`` implement a
``vfork()`` child that runs on the parent's stack instead of a copy, which a
windowed ABI satisfies by construction.  What is missing is the Xtensa
``up_vfork()`` entry point itself.  It has to:

#. spill the register windows with ``xtensa_window_spill()``, so that every
   live frame is in memory at its real address;
#. capture the caller's context (``SYS_save_context`` is the existing
   mechanism, as used by ``up_saveusercontext()``);
#. arrange the child's initial window state so that its first ``retw`` unwinds
   into the application frame that called ``vfork()`` -- which lives *above*
   the parent's reserve and is therefore untouched -- rather than into the
   parent's own kernel-side frames.

Point 3 is the delicate one and is why this is not yet written:  the parent's
remaining frames (``nxtask_start_vfork()`` and the block itself) and the
child's stack both want the memory immediately below the ``vfork()`` call
site.  ``CONFIG_ARCH_VFORK_STACK_RESERVE`` separates them, but the child's
synthetic entry frame then has to be built at the bottom of the reserve rather
than inherited, and that construction is Xtensa-specific.

Note also on ``waitpid()`` after ``vfork()``
============================================

The ``vfork()`` parent is now resumed when the child's TCB is torn down, so by
the time it runs the child is completely gone.  Where the child called
``exec()`` this makes no difference -- ``exec_swap()`` has already given the
loaded program the child's pid, and that program is still running, so
``waitpid()`` behaves normally.  Where the child called ``_exit()``,
``waitpid()`` can only return its status if ``CONFIG_SCHED_CHILD_STATUS`` is
enabled; otherwise it returns ``ECHILD``, because NuttX does not retain the
status of a task that no longer exists.  That is a pre-existing property of
that configuration, not a change:  the previous implementation blocked in a
libc ``waitpid(WNOWAIT)`` and an application's own ``waitpid()`` afterwards hit
the same wall.
