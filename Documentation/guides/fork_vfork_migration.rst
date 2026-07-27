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

That was not ``fork()``.  It was ``vfork()``-with-a-private-stack published
under ``fork()``'s name.  The history says so plainly: the ``fork()`` this
replaces was NuttX's old ``vfork()``, renamed in 2023 without any change of
behaviour.  And the consequence was silent: a program written against POSIX
``fork()`` compiled and ran, and its child's writes quietly landed in the
parent's variables.

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
     - ``CONFIG_ARCH_HAVE_VFORK`` -- no address environment needed
   * - ``task_fork()``
     - child shares memory, private stack copy
     - runs concurrently
     - ``CONFIG_ARCH_HAVE_TASK_FORK`` -- exactly where ``fork()`` existed
       before

``task_fork()`` is the old behaviour under an honest name.  Nothing was lost.

.. note::

   ``fork()`` is provided only where the architecture implements
   ``up_addrenv_fork()`` and therefore selects
   ``CONFIG_ARCH_HAVE_ADDRENV_FORK``; it becomes available architecture by
   architecture as that hook lands.  Check ``CONFIG_ARCH_HAVE_FORK`` in your
   own configuration rather than assuming either way.  Where it is unset,
   ``vfork()`` and ``task_fork()`` are what the configuration offers, and
   ``CONFIG_FORK_IS_TASK_FORK`` keeps the spelling ``fork()`` working for code
   that cannot be changed.

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
   ``vfork()`` + ``exec*()``:  that is exactly what ``vfork()`` is for, and
   unlike ``fork()`` it needs no duplicable address environment.

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

Adding real ``fork()`` to an architecture
-----------------------------------------

Two things are needed, and the second is the one that is easy to miss.

**Implement** ``up_addrenv_fork()``.  It duplicates an address environment:
allocate fresh pages, copy the parent's contents into them, and map them at the
*same* virtual addresses.  ``up_addrenv_clone()`` is not this -- it copies only
the representation and leaves both processes pointing at one set of page
tables.  Then give ``ARCH_HAVE_ADDRENV_FORK`` a ``default y if <arch>`` line in
``arch/Kconfig``, and ``ARCH_HAVE_FORK`` follows.

**Build the child from the caller's saved system call frame.**  In a kernel
build ``fork()`` is reached through a system call, so the return address and
stack pointer the architecture's fork entry point can observe for itself belong
to the *kernel*, not to the caller; a child built from those resumes at a
kernel address on a kernel stack.  The architecture must record the caller's
exception frame when it traps -- ``xcp.sregs`` is the field that exists for
this -- and build the child from that instead, while a kernel thread that calls
the entry point directly still takes the ordinary path.

Nothing else is required:  the ``up_fork()`` entry point and the libc wrapper
are already there and become live automatically.

Note that ``ARCH_HAVE_ADDRENV_FORK`` is about a *per-process* address
environment.  A protected build has one address space carved up once at boot,
whether the boundaries are drawn by an MPU or by a fixed set of MMU mappings;
its ``up_addrenv_*()`` are stubs, and there is no mapping to duplicate at the
same virtual addresses.  ``CONFIG_ARCH_ADDRENV`` being set is therefore not by
itself evidence that ``fork()`` can be provided.  ``vfork()`` and
``task_fork()``, which share the parent's memory, work there as everywhere
else.

Known gaps
==========

``fork()`` **is gained one architecture at a time.**  The generic machinery is
complete -- ``addrenv_fork()``, the ``up_addrenv_fork()`` hook, the syscall, the
libc wrapper and the ``ostest`` case -- so an architecture provides ``fork()``
by implementing ``up_addrenv_fork()`` and selecting
``CONFIG_ARCH_HAVE_ADDRENV_FORK``, with no further generic work.

**A windowed ABI needs its stack rebased, not just copied.**  On Xtensa,
giving a child a relocated copy of the parent's stack takes more than the copy:
the register-window save areas embedded in the stack hold absolute stack
pointers, so each one has to be rebased along with the copy, or the child
reloads a pointer into the *parent's* stack on its very first window underflow.
That rebasing is architecture-specific and belongs with the Xtensa entry points
rather than here.

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
