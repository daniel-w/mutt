#!/usr/bin/perl
#
# A small script to strip out any "illegal" PGP code to make sure it is
# safe for International export.
#
$word = shift;
$illegal = 0;
$count = 0;
while (<>)
{
	if (/^#if/)
	{
		if (/${word}/) { $illegal = 1; }
		if ($illegal) { $count++; }
	}
	elsif ($illegal && /^#endif/)
	{
		$count--;
		if ($count == 0)
		{
			$illegal = 0;
			next;
		}
	}
	print if (! $illegal);
}
