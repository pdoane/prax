<?php
declare(strict_types=1);

// A page that lists the newest posts, with its model above the markup.
namespace Blog;

use DateTimeImmutable;
use PDO;

final class Post
{
    public function __construct(
        public readonly int $id,
        public readonly string $title,
        public readonly DateTimeImmutable $published,
        private array $tags = [],
    ) {}

    public function summary(int $length = 80): string
    {
        return mb_strimwidth($this->title, 0, $length, "\u{2026}");
    }
}

function newestPosts(PDO $db, int $limit = 10): array
{
    $statement = $db->prepare('SELECT id, title, published FROM posts ORDER BY published DESC LIMIT :limit');
    $statement->bindValue(':limit', $limit, PDO::PARAM_INT);
    $statement->execute();
    return array_map(
        fn(array $row) => new Post((int) $row['id'], $row['title'], new DateTimeImmutable($row['published'])),
        $statement->fetchAll(PDO::FETCH_ASSOC)
    );
}

$posts = newestPosts(new PDO('sqlite:blog.db'));
$title = "Newest posts ({${count($posts)}})";
?>
<!DOCTYPE html>
<html lang="en">
<head><title><?= htmlspecialchars($title) ?></title></head>
<body>
    <h1><?= htmlspecialchars($title) ?></h1>
    <?php if (empty($posts)): ?>
        <p class="empty">Nothing yet.</p>
    <?php else: ?>
        <ul>
        <?php foreach ($posts as $post): ?>
            <li><a href="/posts/<?= $post->id ?>"><?= htmlspecialchars($post->summary()) ?></a>
                <time><?= $post->published->format('Y-m-d') ?></time></li>
        <?php endforeach; ?>
        </ul>
    <?php endif; ?>
</body>
</html>
