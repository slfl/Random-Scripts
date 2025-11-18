<!DOCTYPE html>
<html lang="ru">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Шифрование и дешифрование DES</title>
</head>
<body>
    <h1>Шифрование и дешифрование</h1>

    <!-- Форма для шифрования -->
    <h2>Шифрование числа</h2>
    <form method="post">
        <label for="number">Введите число для шифрования:</label><br>
        <input type="text" id="number" name="number" required><br><br>
        <input type="submit" name="encrypt" value="Шифровать">
    </form>

    <?php
    if (isset($_POST['encrypt'])) {
        $data = $_POST['number'];  // Число для шифрования
        $key = "liscjw27";  // Секретный ключ
        $iv = $key;  // Используем тот же ключ как инициализационный вектор (IV)

        // Паддинг данных до нужной длины (кратной 8 байт) с использованием 4 байт
        $padded_data = str_pad($data, 4, "\0");  // Паддинг до длины 4 байта

        // Шифрование с использованием алгоритма DES в режиме CBC
        $encrypted_data = openssl_encrypt($padded_data, 'DES-CBC', $key, OPENSSL_RAW_DATA, $iv);

        // Преобразуем зашифрованные данные в строку в шестнадцатеричном формате
        $encrypted_hex = strtoupper(bin2hex($encrypted_data));

        echo "<h3>Зашифрованное значение:</h3>";
        echo "<p>$encrypted_hex</p>";
    }
    ?>

    <hr>

    <!-- Форма для дешифрования -->
    <h2>Дешифрование зашифрованного значения</h2>
    <form method="post">
        <label for="encrypted">Введите зашифрованное значение:</label><br>
        <input type="text" id="encrypted" name="encrypted" required><br><br>
        <input type="submit" name="decrypt" value="Дешифровать">
    </form>

    <?php
    if (isset($_POST['decrypt'])) {
        $encrypted_hex = $_POST['encrypted'];  // Зашифрованное значение
        $key = "liscjw27";  // Секретный ключ
        $iv = $key;  // Используем тот же ключ как инициализационный вектор (IV)

        // Преобразуем шестнадцатеричное значение обратно в бинарное
        $encrypted_data = hex2bin($encrypted_hex);

        // Дешифрование с использованием алгоритма DES в режиме CBC
        $decrypted_data = openssl_decrypt($encrypted_data, 'DES-CBC', $key, OPENSSL_RAW_DATA, $iv);

        // Удаляем паддинг (4 байта)
        $decrypted_data = rtrim($decrypted_data, "\0");

        echo "<h3>Исходное число:</h3>";
        echo "<p>$decrypted_data</p>";
    }
    ?>

</body>
</html>
