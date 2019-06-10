#!/bin/sh

test_description='rebase should reread the todo file if an exec modifies it'

. ./test-lib.sh

todo=.git/rebase-merge/git-rebase-todo

test_expect_success 'rebase exec modifies rebase-todo' '
	test_commit initial &&
	git rebase HEAD -x "echo exec touch F >>$todo" &&
	test -e F
'

test_expect_success SHA1 'loose object cache vs re-reading todo list' '
	GIT_REBASE_TODO=$todo &&
	export GIT_REBASE_TODO &&
	write_script append-todo.sh <<-\EOS &&
	# For values 5 and 6, this yields SHA-1s with the same first two digits
	echo "pick $(git rev-parse --short \
		$(printf "%s\\n" \
			"tree $EMPTY_TREE" \
			"author A U Thor <author@example.org> $1 +0000" \
			"committer A U Thor <author@example.org> $1 +0000" \
			"" \
			"$1" |
		  git hash-object -t commit -w --stdin))" >>$GIT_REBASE_TODO

	shift
	test -z "$*" ||
	echo "exec $0 $*" >>$GIT_REBASE_TODO
	EOS

	git rebase HEAD -x "./append-todo.sh 5 6"
'

test_expect_success 'rebase exec respects rebase.missingCommitsCheck = ignore' '
	test_config rebase.missingCommitsCheck ignore &&
	git rebase HEAD~2 --keep-empty -x "echo >$todo" &&
	test 5 = $(git cat-file commit HEAD | sed -ne \$p)
'

cat >expect <<EOF
Warning: some commits may have been dropped accidentally.
Dropped commits (newer to older):
 - $(git rev-list --pretty=oneline --abbrev-commit -1 HEAD@{2})
To avoid this message, use "drop" to explicitly remove a commit.

Use 'git config rebase.missingCommitsCheck' to change the level of warnings.
The possible behaviours are: ignore, warn, error.
EOF

test_expect_success 'rebase exec respects rebase.missingCommitsCheck = warn' '
	test_config rebase.missingCommitsCheck warn &&
	git reset --hard HEAD@{2} &&
	git rebase HEAD~2 --keep-empty -x "echo >$todo" 2>actual.2 &&
	head -n8 actual.2 | tail -n7 >actual &&
	test_i18ncmp expect actual &&
	test 5 = $(git cat-file commit HEAD | sed -ne \$p)
'

test_expect_success 'rebase exec respects rebase.missingCommitsCheck = error' '
	test_config rebase.missingCommitsCheck error &&
	git reset --hard HEAD@{2} &&
	test_must_fail git rebase HEAD~2 --keep-empty -x "echo >$todo" 2>actual.2 &&
	head -n8 actual.2 | tail -n7 >actual &&
	test_i18ncmp expect actual &&
	echo drop $(git rev-list --pretty=oneline -1 HEAD@{1}) >$todo &&
	git rebase --continue 2>actual &&
	test 5 = $(git cat-file commit HEAD | sed -ne \$p) &&
	test_i18ngrep \
		"Successfully rebased and updated refs/heads/master" \
		actual
'

test_done
